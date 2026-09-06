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
#include "map.h"
#include "npc.h"
#include "bot.h"
#include "groups.h"
#include "corpse.h"
#include "../common/crc32.h"
#include "../common/compression.h"
#include "../common/eqemu_logsys.h"
#include "../common/patches/trilogy_structs.h"
#include "../common/eq_packet_structs.h"
#include "../common/eq_constants.h"
#include "../common/emu_constants.h"
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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <utility>
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
// Client -> zone: sent at T+30s from OP_Camp if the client's local countdown
// completed without interruption.  This is the authoritative "camp finished"
// signal in the Verant-era protocol; EQClassic ProcessOP_DeleteSpawn
// (LS/zone/client_process.cpp:2241) saves, broadcasts DeleteSpawn to nearby
// clients, sends 0x5941, then Disconnects.  We do the same via CompleteCamp.
static constexpr uint16_t ZN_OP_DeleteSpawn = 0x5021;
// Client -> zone: fragmented ~8100-byte PlayerProfile dump sent alongside
// OP_DeleteSpawn during camp-out (and again on zone transitions).  EQClassic
// just calls Save() and does not ack — the client does not wait for a reply.
static constexpr uint16_t ZN_OP_PlayerSave2 = 0x5521;
// Sibling of ZN_OP_PlayerSave2: EQClassic dispatches BOTH 0x5421 and 0x5521 to
// the same ProcessOP_PlayerSave (Client_Packet.cpp:157-158); Harakiri's note is
// that the client picks one of the two "before zoning or every couple minutes".
// We only mirror the Save() on 0x5521 (0x5421 previously fell through to the
// UNHANDLED logger); 0x5421 is wired up so the air probe sees that sample too.
static constexpr uint16_t ZN_OP_PlayerSave  = 0x5421;
static constexpr uint16_t ZN_OP_ZoneChange  = 0xa320; // bidirectional: ZoneChange_Struct (68 bytes)

// Combat / looting opcodes
// Source: EQClassic/Common/Include/eq_opcodes.h
static constexpr uint16_t ZN_OP_Death          = 0x4a20; // client -> zone: Death_Struct (20 bytes) — client-initiated death
// Environmental damage, client -> zone.  v29c aliases OP_Action / OP_Damage /
// OP_EnvDamage onto the SAME opcode (EQClassic eq_opcodes.h:145 and :246), so a
// 28-byte inbound 0x5820 is the client telling us it hurt itself: Action_Struct
// with type = 0xFA lava / 0xFB drowning / 0xFC falling / 0xFD trap and the amount
// in `damage`.  0x1e20 (OP_ENVDAMAGE2) is the 36-byte sibling the client sends in
// the same tick; EQClassic relays it to everyone so observers see the hit, and
// never treats it as damage — it carries the same type and amount, so applying
// both would charge the player twice.
static constexpr uint16_t ZN_OP_EnvDamage      = 0x5820; // client -> zone: Action_Struct (28 bytes)
static constexpr uint16_t ZN_OP_EnvDamage2     = 0x1e20; // client -> zone: 36 bytes, rebroadcast only
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

// Social / party commands (client -> zone).
// Wire sizes below are measured from a live v29c session, not inferred.  Every
// struct except LFG is byte-identical to its modern EQEmu counterpart, so these
// translate to a plain memcpy plus a dispatch into the existing handler.
//
//   0x0022 /assist        4 B  {uint32 entity_id}          == EntityId_Struct
//   0xfe21 /target <name> 4 B  {uint32 entity_id}          == ClientTarget_Struct
//                              (client resolves the name; we get an id)
//   0xe721 /random        8 B  {uint32 low, high}          == RandomReq_Struct
//   0x3121 /split        16 B  {uint32 pp, gp, sp, cp}     == Split_Struct
//   0xda21 /yell          4 B  {uint32 entity_id}          — relayed to nearby
//   0xf021 /lfg          36 B  {char name[32]; int32 on}   != EQEmu LFG_Struct
//   0xb720 /consent       8 B  {char name[]}               == Consent_Struct
// 0xff21 /options — chat and combat message filters.  60 bytes = 15 int32
// toggles, and the busiest inbound opcode the client sends that we do not
// consume — every preference set in the Options panel used to be discarded.
//
// The slot ORDER is documented nowhere: EQClassic declares this struct as
// `int8 unknown[60]` (eq_packet_structs.h:1622) and never decodes it, and
// EQEmu's SetServerFilter_Struct is a 29-entry array on its own ordering
// (eqFilterType, eq_constants.h:736-764).  Mapping it by guesswork would
// silently mis-filter real messages, which is worse than the current no-op —
// so the slots were pinned empirically before anything was applied.  See
// HandleServerFilter for the resulting map and the procedure that produced it.
static constexpr uint16_t ZN_OP_SetServerFilter = 0xff21;
static constexpr uint16_t ZN_OP_Assist         = 0x0022;
static constexpr uint16_t ZN_OP_TargetByName   = 0xfe21;
static constexpr uint16_t ZN_OP_Random         = 0xe721;
static constexpr uint16_t ZN_OP_SplitMoney     = 0x3121;
static constexpr uint16_t ZN_OP_Yell           = 0xda21;
static constexpr uint16_t ZN_OP_LFG            = 0xf021;
static constexpr uint16_t ZN_OP_ConsentRequest = 0xb720;

// Guild management (client -> zone unless noted).
//
// Names come from EQClassic Common/Include/eq_opcodes.h; the two it does not
// declare (0x0422, 0x1a21) are named in EQMacEmu's patch_Trilogy.conf, which is
// a name table only.  EQClassic's OP_GuildMOTD covers 0x0322 alone and treats
// it as bidirectional, so we do the same and never emit 0x0422 — its payload
// shape is unverified and guessing at it risks the client, while the MOTD text
// itself reaches the player as guild-channel chat, exactly as EQClassic does it
// (LS/zone/client_process.cpp:2531 Message(MT_Guild, "Guild MOTD: %s")).
//
//   0x1721 OP_GuildInvite        68 B  GuildCommand_Struct       bidirectional
//   0x1821 OP_GuildInviteAccept  68 B  GuildInviteAccept_Struct
//   0x1921 OP_GuildRemove        68 B  GuildCommand_Struct
//   0x0322 OP_GuildMOTD          var   name + motd text          bidirectional
//   0x9521 OP_GuildLeader        var   bare null-terminated name
//   0x1a21 OP_GuildDelete        var   payload unused
//   0x6f21 OP_GuildWar           var   never implemented in this era
//   0x9121 OP_GuildPeace         var   never implemented in this era
//   0x2821 OP_GetGuildsList       4 B  guild id the client cannot resolve
static constexpr uint16_t ZN_OP_GuildInvite       = 0x1721;
static constexpr uint16_t ZN_OP_GuildInviteAccept = 0x1821;
static constexpr uint16_t ZN_OP_GuildRemove       = 0x1921;
static constexpr uint16_t ZN_OP_GuildMOTD         = 0x0322;
static constexpr uint16_t ZN_OP_GuildLeader       = 0x9521;
static constexpr uint16_t ZN_OP_GuildDelete       = 0x1a21;
static constexpr uint16_t ZN_OP_GuildWar          = 0x6f21;
static constexpr uint16_t ZN_OP_GuildPeace        = 0x9121;
// Client -> zone, 4 B: "I have a guild id I cannot resolve, send me the table."
// EQClassic does not declare this opcode at all; the name is from EQMacEmu's
// patch_Trilogy.conf, and the meaning is from a live session — it arrived with
// payload 65 01 00 00 (= 357) moments after guild 357 was created, on a client
// whose char-select table predated it.  Answered with the full 0x9221 table.
static constexpr uint16_t ZN_OP_GetGuildsList     = 0x2821;

// 0xf420 /who all.  76 B Trilogy::structs::WhoAll_Struct.  A bare /who sends
// nothing — v29c builds the zone roster client-side, like the Tracking list.
//
//   /*000*/ char  whom[32]   name / zone / guild substring; empty = no filter
//   /*032*/ int16 wrace      0xFFFF = no race filter
//   /*034*/ int16 wclass     0xFFFF = no class filter
//   /*036*/ int16 firstlvl   0xFFFF = no level filter
//   /*038*/ int16 secondlvl
//   /*040*/ int16 gmlookup   0xFFFF = not /who all gm
//   /*042*/ int16 wguild     guild ID, 0xFFFF = no guild filter.  EQClassic
//                            folds this into its trailing unknown[34]; a bare
//                            /who all shows SIX 0xFFFF words at 32..43, one
//                            more filter slot than it declares.  Confirmed a
//                            guild ID and not a flag: /who all "Fire" against a
//                            server whose guild 1 is "Fire and Fury" arrives as
//                            whom="Fire" wguild=1 -- the client resolves the
//                            name against the list it got at login and sends
//                            both halves.
//
// Every "no filter" sentinel is 0xFFFF, and EQEmu's Who_All_Struct compares its
// uint32 fields against 0xFFFF as well (world/clientlist.cpp:597-612), so those
// zero-extend unchanged.  guildid is the exception -- EQEmu's "none" for that
// one is 0xFFFFFFFF -- so it is mapped explicitly in HandleWhoAll.
static constexpr uint16_t ZN_OP_WhoAll         = 0xf420;

// 0x4121 OP_ZoneEntryResend.  2 B: { int16 spawn_id }.  Client -> zone.
//
// The client asking us to send a spawn again.  It arrives as a sweep: one
// packet per entry in the client's spawn list, in ascending id order, and the
// whole sweep repeats a few seconds later if nothing comes back.  Live capture
// of a single burst: 21 ids (30, 31, 35..37, 39..42, 44..50, 52..56) requested
// three times over about one second, immediately after a batch of 0x2b20
// DeleteSpawn went out.
//
// This is the client's own repair path for spawn desync, and it has been
// discarded for the entire life of the branch -- 140 requests in one logged
// session, the loudest thing v29c says that we do not answer.  Worth stating
// plainly: every "NPC is invisible" and "clicked it, nothing happened" bug this
// branch has chased had the client asking us to fix it, and being ignored.
//
// The name is from EQMacEmuTrilogy's patch_Trilogy.conf, which is the only
// place either reference tree names it -- EQClassic has neither the opcode nor
// the concept.  Only the name was taken; the handler below is built on our own
// spawn senders.
static constexpr uint16_t ZN_OP_ZoneEntryResend = 0x4121;

// 0x1f20 OP_SetRunMode.  4 B, byte-identical to EQEmu's SetRunMode_Struct
// { uint8 mode; uint8 unknown[3] } — the wire only ever carries 01 00 00 00 or
// 00 00 00 00, which is exactly that shape.
//
// EQClassic named it OP_MovementUpdate and left the case empty with the comment
// "Noticed this was going unknown opcode between run/walk"
// (Zone/Source/client_process.cpp:6406), so the semantics are settled by its own
// note even though it never used the value.
//
// We should: Client::runmode is initialised false (zone/client.cpp:242) and,
// with nothing dispatching this, stays false for the whole session on a Trilogy
// client.  Two places read it — Mob::GetMovementSpeed picks GetBaseWalkspeed()
// over GetBaseRunspeed() for a Client (zone/mob.cpp:907), so the server models
// every v29c player as walking regardless of what they are doing; and
// client_mods.cpp:1680 gates its is_running term on it.
static constexpr uint16_t ZN_OP_SetRunMode      = 0x1f20;

// 0x4721 OP_ClientError.  92 B.  The client reporting its OWN faults — EQClassic
// (Common/Include/eq_opcodes.h:222) describes it as "client sents this when an
// error client side happend i.e. a stackable item without charges sent to the
// client, or an invalid item (got a bogus item) etc".
//
// Neither reference tree declares a struct for it, so this is logged rather than
// parsed.  What the captures do show, consistently:
//
//   /*000*/ int32  0xFFFFFFFC   (-4)      constant across every sample
//   /*004*/ int32  0xFFFFFFFF   (-1)      constant across every sample
//   /*008*/ int32  error code              observed 2 and 6
//   /*012*/ ...    context, code-dependent
//
// Code 6 carries what look like client-side code addresses (0x00410B5F,
// 0x0FE7A748) followed by four floats that read as a position — 136.0, 167.0,
// -52.6, 92.0.  Code 2 carries a system-DLL address (0x77062B60) and little
// else.  Both smell like fault reports with a return address, which is only
// decodable against the client binary.
//
// Logging the whole payload is the useful move regardless: this is the client
// telling us something we sent it was malformed, and on a branch whose history
// is largely inventory and spawn desync that is signal worth keeping.
static constexpr uint16_t ZN_OP_ClientError     = 0x4721;

// 0x1120 OP_PetitionRefresh.  0 B, fires on every zone-in.  Source: EQClassic
// Common/Include/eq_opcodes.h:262.  The client polling for petition-queue
// changes; with no petition UI wired up there is nothing to answer with, so it
// is consumed silently rather than left to the unhandled logger.
static constexpr uint16_t ZN_OP_PetitionRefresh = 0x1120;

// Spell opcodes (bidirectional)
// Source: EQClassic/Common/Include/eq_opcodes.h + trilogy_structs.h comments
// ZN_OP_Buff is bidirectional: outbound it carries duration refreshes and the
// buff bar (TrilogyClient::HandleBuff); inbound it is the client asking to drop
// a buff, which is what right-clicking a buff icon sends.
static constexpr uint16_t ZN_OP_Buff          = 0x3221; // bidirectional: Buff_Struct (20 bytes)
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

// /surname command
// Source: EQClassic/Common/Include/eq_opcodes.h + client_process.cpp:2196
static constexpr uint16_t ZN_OP_Surname       = 0xc421; // client -> zone: Surname_Struct (56B, /surname submit)
static constexpr uint16_t ZN_OP_GMSurname     = 0x6e21; // zone -> nearby: GMSurname_Struct (94B, refresh nameplates)

// Socials (/bow, /wave, /emote ...).  EQClassic splits one user action into two
// independent packets and the client sends whichever apply:
//   OP_Social_Text   — the chat line.  Payload is the PREDICATE ONLY (" bows.");
//                      the server prepends the actor's name.
//   OP_Social_Action — the body animation.  Same 12-byte struct we already emit
//                      outbound for OP_Animation, so `action` sits at byte 4.
// Source: EQClassic/Common/Include/eq_opcodes.h:265-266,
//         client_process.cpp:4203 (ProcessOP_Social_Text) and :6525 (OP_Social_Action).
static constexpr uint16_t ZN_OP_SocialText    = 0x1520; // client <-> nearby: bare NUL-terminated string
static constexpr uint16_t ZN_OP_SocialAction  = 0x9f20; // client -> zone: Attack_Struct shape, anim id at +4

// Resurrection (all 160B Resurrect_Struct).
// Source: EQClassic Common/Include/eq_opcodes.h + eq_packet_structs.h
// - ZN_OP_RezzRequest  is bidirectional but we only translate zone->client
//   (server-side spell effect drives the request; inbound 0x2a21 from the
//   caster's client would double-fire and is silently dropped).
// - ZN_OP_RezzAnswer   is the corpse owner's yes/no reply (action=1/0).
// - ZN_OP_RezzComplete terminates the pending-rez state on the client.
static constexpr uint16_t ZN_OP_RezzRequest   = 0x2a21; // bidirectional; used server -> client
static constexpr uint16_t ZN_OP_RezzAnswer    = 0x9b21; // client -> zone: accept / decline
static constexpr uint16_t ZN_OP_RezzComplete  = 0xec21; // zone -> client: rez finished

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

// Tradeskill / world container opcodes
// Source: EQClassic/Common/Include/eq_opcodes.h
//   OP_CraftingStation (0xd720) is BIDIRECTIONAL on the same wire opcode:
//     zone -> client: open container UI (ClickObjectAck_Struct, 20 B)
//     client -> zone: player closed UI; server snapshots stationItems back onto
//                     the Object (same struct echoed back, open=0).
//   OP_StationItem (0xfb20) carries the full 292 B ClassicItem_Struct, one packet
//     per occupied slot; the item's equipSlot field holds 0..9 (the bag interior
//     index), NOT the 4000-4009 wire slot used by OP_MoveItem.
//   OP_TradeSkillCombine (0x0521) carries Combine_Struct (32 B) when the player
//     hits the Combine button — same struct/opcode whether the container is on
//     the ground (worldobjecttype != 0, containerslot = 1000) or in inventory.
static constexpr uint16_t ZN_OP_CraftingStation   = 0xd720;
static constexpr uint16_t ZN_OP_StationItem       = 0xfb20;
static constexpr uint16_t ZN_OP_TradeSkillCombine = 0x0521;

// Group opcodes
// Source: EQMacEmuTrilogy patch_Trilogy.conf + EQClassic/LS branch eq_opcodes.h
// (EQClassic/Common/Include uses different values for Invite/Follow/Update —
// do not use as reference, see patch_Trilogy.conf comment for context.)
static constexpr uint16_t ZN_OP_GroupInvite        = 0x3e20; // bidirectional: GroupInvite_Struct (95B)
static constexpr uint16_t ZN_OP_GroupInvite2       = 0x4020; // client -> zone: alt invite form
static constexpr uint16_t ZN_OP_GroupFollow        = 0x4220; // client -> zone: accept invite, GroupFollow_Struct (60B)
                                                            // NOT 0x3d20 — EQMacEmuTrilogy patch is wrong here, EQClassic Common is right
static constexpr uint16_t ZN_OP_GroupCancelInvite  = 0x4120; // bidirectional: decline, GroupInviteDecline_Struct (65B)
static constexpr uint16_t ZN_OP_GroupDisband       = 0x4420; // client -> zone: leave / kick / disband, GroupDisband_Struct (60B)
static constexpr uint16_t ZN_OP_GroupUpdate        = 0x2620; // zone -> client: GroupUpdate_Struct (228B)

// Inspect opcodes (right-click another player → equipment window + about-me text)
// Source: EQClassic/Common/Include/eq_opcodes.h + EQClassic/Zone/Source/client_process.cpp:4590
//   Model: pure client-to-client relay.  Server does NOT construct the answer
//   payload — the v29c client builds it itself from local equipment + inspect
//   text, and the server just forwards the packet to the requester/target.
//
//   OP_InspectRequest (0xb520) — 8 B: {int32 TargetID, int32 PlayerID}
//                                     TargetID = who is being inspected
//                                     PlayerID = who is inspecting
//   OP_InspectAnswer  (0xb620) — 1044 B: {int32 TargetID, int32 PlayerID,
//                                         uint8 payload[1036]}
//                                     TargetID/PlayerID SWAPPED on the answer
//                                     (TargetID = requester, PlayerID = responder)
//                                     payload = opaque to server — item slots
//                                     + about-me text; layout TBD from capture
static constexpr uint16_t ZN_OP_InspectRequest = 0xb520;
static constexpr uint16_t ZN_OP_InspectAnswer  = 0xb620;

// EQNetwork header flags (identical to world handler)
static constexpr uint8_t HDR0_ARQ      = 0x02;
static constexpr uint8_t HDR0_FRAGMENT = 0x08;
static constexpr uint8_t HDR0_ASQ      = 0x10;
static constexpr uint8_t HDR0_SEQSTART = 0x20;
static constexpr uint8_t HDR1_ARSP     = 0x04;

// ============================================================
// Speed / animation wire encoding for v29c
// ============================================================
//
// The Trilogy client renders the *local player's* movement speed from the
// walkspeed/runspeed floats in their own Spawn_Struct (sent once at zone-in).
// EQClassic Client::Client() seeds these to (0.46, 0.70). EQEmu's modern
// baseline is 1.25 runspeed — sending that to v29c makes the player feel
// ~2x faster than authentic Verant-era movement.
//
// These two constants are the single tuning knob for player base speed.
// To match Titanium-style feel instead, raise to (0.70, 1.25). To make the
// player faster again, raise further.
//
// SoW / movement-speed buffs still work because v29c's client computes the
// modifier locally from the SE_MovementSpeed buff effect. GM-speed currently
// does NOT visibly speed the player up locally (no v29c speed-update packet
// exists; only the spawn struct walk/run floats and locally-applied buffs
// drive the player's own animation rate). Wiring GM-speed is a follow-up.
static constexpr float kTrilogyPlayerWalkSpeed = 0.46f;
static constexpr float kTrilogyPlayerRunSpeed  = 0.70f;

// Full breath meter, in the v29c client's own units, for PlayerProfile.air_supply
// (struct byte 2633).  Not a guess: this is the value the client reports for itself
// while standing on dry land, read straight out of one of its own PP uploads.  See
// the field comment in trilogy_structs.h and [TrilogyAirProbe] for how it was pinned.
static constexpr uint16_t kTrilogyFullAirSupply = 45;

// EQEmu stores speeds as int = float × 40 (see Mob::Mob in mob.cpp:192).
// Reverse the conversion for the Trilogy spawn wire format.
static inline float ToTrilogySpeed(int eqemu_speed)
{
	return static_cast<float>(eqemu_speed) / 40.0f;
}

// ============================================================
// SpawnPositionUpdate delta_x / delta_y / delta_z 10-bit bitfield
// helpers.  The wire layout (trilogy_structs.h:573-577) is:
//   /*011*/  int32 delta_y:10, spacer1:1, delta_z:10, spacer2:1, delta_x:10;
// The struct comment explicitly warns against relying on the compiler's
// bitfield packing — different compilers / packing pragmas can produce
// different byte orders.  Use these explicit shifts so the wire format is
// stable across MSVC / GCC / Clang.
//
// Layout (LSB-first, little-endian 32-bit word at offset 11):
//   bits  0-9   : delta_y  (signed 10-bit, range -512..511)
//   bit  10     : spacer1
//   bits 11-20  : delta_z
//   bit  21     : spacer2
//   bits 22-31  : delta_x
static inline uint32_t PackDelta10(int32_t v)
{
	if (v >  511) v =  511;
	if (v < -512) v = -512;
	return static_cast<uint32_t>(v) & 0x3FF;
}
static inline int32_t UnpackDelta10(uint32_t raw)
{
	raw &= 0x3FF;
	// Sign-extend 10-bit to 32-bit: bit 9 is the sign bit.
	return (raw & 0x200) ? static_cast<int32_t>(raw | 0xFFFFFC00u)
	                     : static_cast<int32_t>(raw);
}
static inline void WriteDeltaBitfield(Trilogy::structs::SpawnPositionUpdate_Struct* upd,
                                      int32_t dx, int32_t dy, int32_t dz)
{
	const uint32_t bits =  PackDelta10(dy)
	                    | (PackDelta10(dz) << 11)
	                    | (PackDelta10(dx) << 22);
	uint8_t* p = reinterpret_cast<uint8_t*>(upd);
	std::memcpy(p + 11, &bits, 4);
}
static inline void ReadDeltaBitfield(const Trilogy::structs::SpawnPositionUpdate_Struct* upd,
                                     int32_t& dx, int32_t& dy, int32_t& dz)
{
	uint32_t bits;
	const uint8_t* p = reinterpret_cast<const uint8_t*>(upd);
	std::memcpy(&bits, p + 11, 4);
	dy = UnpackDelta10(bits);
	dz = UnpackDelta10(bits >> 11);
	dx = UnpackDelta10(bits >> 22);
}

// Server-side velocity (EQ-units/sec) → 10-bit wire delta value.
//
// FORCED TO 0 (2026-06-27): matches EQClassic's effective wire-out behavior.
// Reread EQClassic mob.cpp:581-583 after empirical failures at scale=10 and
// scale=1.  Their outbound encoder does `spu->delta_y = stored / 125` where
// `stored` is:
//   - For NPCs: 0 (never written by their server code).
//   - For PCs observed by others: cu->delta_y (the small wire value the v29c
//     client itself sent inbound, typically 5-25), divided by 125 → also 0.
// So the v29c client receives delta = 0 for ALL entities from EQClassic's
// server.  delta_x/y/z is an INBOUND-only field in practice — the client
// uses it locally to compute its own player motion, but receivers do NOT
// trust it for inter-entity extrapolation.
//
// Both empirical iterations (scale=10 and scale=1) reproduced the same
// "strafing as if destination is evolving" symptom — varying-direction
// delta each heartbeat made the client extrapolate erratically in
// directions that didn't match the path.  Magnitude shrank between
// iterations but the directional confusion persisted, confirming the
// problem is the *fact* of non-zero delta rather than its size.
//
// Keep at 0 unless we positively identify a v29c client behavior that
// requires non-zero delta_x/y from the server side.  Residual position-snap
// jagginess from the 4 Hz heartbeat cadence is a separate problem to solve
// via higher heartbeat rate or event-driven updates.
static constexpr float    kVelocityWireScale = 0.0f;
static constexpr uint64_t kDeltaDebugMs      = 1000;   // 1 sec per log line

// IMPORTANT: SpawnPositionUpdate_Struct::delta_heading is NOT an
// interpolation hint between sent heading values — it is the SPIN-STUN
// rotation-per-tick rate.  EQClassic's mob.cpp:570-574 sets it to 100 only
// while NPC::GetSpin() is true (the spin-stun spell effect):
//   if (IsNPC() && CastToNPC()->GetSpin()) spu->delta_heading = 100;
//   else                                   spu->delta_heading = delta_heading;
// EQClassic's stored `delta_heading` on a Mob is 0 except when the player
// CLIENT sends a non-zero delta_heading in cu->delta_heading (player keyboard
// turning, fed back to other clients).  For NPCs the value is always 0.
//
// A previous attempt here computed delta_heading from the per-heartbeat
// heading change to "smooth" pathgrid rotation; the v29c client interpreted
// those small non-zero values as a sustained spin rate, producing endless
// rotation on guards arriving at waypoints and on NPCs answering /hail.
// Keep this function in the codebase as a building block for when we wire
// the spin-stun spell effect (the only legitimate use of non-zero
// delta_heading for NPCs); do NOT call it from the regular heartbeat path.
//
// The shortest-path angular fold + ±127 clamp below would still be the
// right encoding for a "turn this far over the next tick" semantic if v29c
// ever exposed one.  It does not — kept here for documentation and future
// spin-stun wiring.
[[maybe_unused]] static int8_t ComputeTrilogyDeltaHeading(int8_t prev_heading_wire,
                                                          int8_t cur_heading_wire,
                                                          bool has_prev)
{
	if (!has_prev) return 0;
	const int prev = static_cast<int>(static_cast<uint8_t>(prev_heading_wire));
	const int cur  = static_cast<int>(static_cast<uint8_t>(cur_heading_wire));
	int diff = cur - prev;
	if (diff >  128) diff -= 256;
	if (diff < -128) diff += 256;
	if (diff >  127) diff =  127;
	if (diff < -127) diff = -127;
	return static_cast<int8_t>(diff);
}

// Encode a Mob's current movement speed into Trilogy's SpawnPositionUpdate
// anim_type byte.
//
// EQClassic semantics (Zone/Source/npc.cpp:1716-1726):
//   stationary (eqemu_anim == 0)                       → 0
//   walking    (eqemu_anim ≤ walkspeed +1 fudge)       → walkspeed_float × 4
//   running    (otherwise)                             → runspeed_float  × 7
//   fleeing                                            → negated walk/run
//
// The v29c client treats anim_type as an unsigned-ish "velocity" — magnitude
// drives leg-cycle playback rate and there's a threshold (≈6) above which the
// client switches from the walk animation to the run animation.  The client's
// extrapolation rate per unit anim is much larger than the EQClassic
// server-side formula `npc_walking_units_per_second = anim * 2.3/5` suggests
// (that's the SERVER's rate, not the client's interpretation).  Empirically
// confirmed 2026-06-27 by an "option 2" experiment that scaled anim up to
// match EQEmu's faster server motion: NPCs zoomed across zones at apparent
// hundreds of units/sec, proving the client applies a much higher
// per-anim-unit multiplier than naive math suggests.  Stick to EQClassic's
// small byte values even though they mean the client's extrapolation
// undershoots EQEmu's actual server motion (residual jagged trajectory).
//
// We deviate from EQClassic's literal multiplier-of-4 for the walk case
// because EQEmu's NPC walkspeed scale is significantly slower than EQClassic's:
//   - EQClassic default NPC walkspeed_float = 0.70  → byte = 0.70 × 4 ≈ 2
//   - EQEmu default NPC walkspeed_float    = 0.45  → byte = 0.45 × 4 ≈ 1
// At byte=1 the v29c client plays the walk cycle at ~half EQClassic's rate,
// so EQEmu NPCs visually shuffle when patrolling. Using multiplier 7 for
// walking and clamping the result to [2,5] keeps the byte inside the walk-
// animation band while restoring a perceptible cycle rate at EQEmu speeds.
// (5 stays safely below the ≈6 run-cycle threshold; 2 is the slowest byte
// where the leg cycle is clearly animated rather than crawling.)
//
// `eqemu_anim` is the EQEmu integer speed value (28 walk / 50 run at base;
// modified by spell/item bonuses).  Pass GetWalkspeed() / GetRunspeed() for
// the heartbeat path where we don't have an incoming animation; pass the
// PlayerPositionUpdateServer_Struct::animation field for the MovementManager
// translation path (HandleClientUpdate).
//
// Result magnitude is clamped to [1,127] so anim_type==0 always means "stop"
// and never collides with a sign-magnitude walk/run.  Bots have their walk/
// run scaled by 1.7857 (bot.h:226-227); the fudge in the walk comparison
// absorbs the rounding boundary cleanly without special-casing the class.
int8_t TrilogyZoneServer::EncodeTrilogyAnim(Mob* m, int eqemu_anim)
{
	if (!m || eqemu_anim == 0) return 0;

	const int abs_anim = std::abs(eqemu_anim);
	const int walk     = m->GetWalkspeed();
	const bool is_walk = (walk > 0 && abs_anim <= walk + 1);

	const float speed_f = static_cast<float>(abs_anim) / 40.0f;
	int byte;
	// 2026-06-27: split multipliers — walk ×10, run ×12 — paired with the
	// 3× spawn walkspeed/runspeed bump.  Empirical observations:
	//   walk: typical NPC (speed_int=18) → byte=4.  Plays walk cycle,
	//         body translates at server walk rate.  Smooth, no snap.
	//   run:  typical NPC (speed_int=50) → byte=15.  Must clear the
	//         walk-cycle ceiling (~walkspeed × 9 ≈ 12 with our 1.35
	//         walkspeed) to trigger v29c's run animation cycle.  ×7 and
	//         ×10 both produced bytes (8 and 12 respectively) that stayed
	//         inside walk-cycle, giving "body running with walking legs"
	//         symptom.  ×12 lands at byte=15, clear of the threshold.
	// Body translation rate for both cycles is driven by spawn-time
	// walkspeed/runspeed (the 3× bump), separate from the byte's
	// cycle-selection role.
	if (is_walk) {
		byte = static_cast<int>(speed_f * 10.0f);
		if (byte < 2)  byte = 2;
		if (byte > 10) byte = 10;
	} else {
		byte = static_cast<int>(speed_f * 12.0f);
		if (byte < 1)   byte = 1;
		if (byte > 127) byte = 127;
	}
	return static_cast<int8_t>(eqemu_anim < 0 ? -byte : byte);
}

// ============================================================
// EncodeTrilogyDelta — thin static wrapper around the file-local
// WriteDeltaBitfield helper so TrilogyClient (in trilogy_client.cpp)
// can populate the v29c per-tick velocity field from EQEmu's
// MobMovementManager-fed deltas.  See declaration in trilogy_zone.h
// for the why.  The void* parameter is the
// Trilogy::structs::SpawnPositionUpdate_Struct (avoiding a struct
// include in trilogy_zone.h to keep that header lean).
// ============================================================
void TrilogyZoneServer::EncodeTrilogyDelta(void* spawn_position_update,
                                           int32_t dx, int32_t dy, int32_t dz)
{
	WriteDeltaBitfield(
		static_cast<Trilogy::structs::SpawnPositionUpdate_Struct*>(spawn_position_update),
		dx, dy, dz);
}

// ============================================================
// BuildTrilogyCorpseName — convert a player corpse's internal name
// (e.g. "Bleargh's_corpse0") to the backtick+underscore wire format
// while preserving the trailing number suffix for uniqueness.
// The v29c client strips trailing digits from display names, so
// "Bleargh`s_corpse0" and "Bleargh`s_corpse1" both display as
// "Bleargh`s corpse" but remain distinct entities for illusion matching.
// ============================================================
// ============================================================
// IsTrilogyTimeLocked — true when this zone's sky should be pinned
// to a fixed EQ time via the Zone:TrilogyAirplaneLockTime* rules.
// Only original airplane (Plane of Sky) matches for now.
// ============================================================
static bool IsTrilogyTimeLocked(const char* short_name)
{
	if (!short_name || !RuleB(Zone, TrilogyAirplaneLockTime)) {
		return false;
	}
	return strcasecmp(short_name, "airplane") == 0;
}

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
// See trilogy_zone.h for the full rationale and kill-switch instructions.
const char* TrilogyWireName(Mob* m)
{
	if (!m) return "";
	return IsPlayerRace(m->GetRace()) ? m->GetName() : m->GetCleanName();
}

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
		s.trilogy_client  = nullptr;
		s.eqemu_entity_id = 0;
		entity_list.RemoveMob(id); // removes from client_list + mob_list, calls safe_delete (~Client decrements numclients)
	} else if (s.counted_in_zone && numclients > 0) {
		--numclients;
	}
	LogInfo("[TrilogyZone] Session removed, numclients={}", numclients);
	m_sessions.erase(it);
	if (zone) zone->SetHasActiveTrilogySessions(!m_sessions.empty());
}

// ============================================================
// EnterLinkdead — begin the grace window for a session whose client is gone.
//
// EQClassic (LS/zone/client_process.cpp:6769): on a dead connection it sets
// CLIENT_LINKDEAD, starts server AI for CLIENT_LD_TIMEOUT and sends
// AT_LD=1.  CLIENT_LINKDEAD still counts as in-zone (client.h:96), so the
// body keeps taking aggro and can die rather than blinking out mid-fight.
//
// Called from two places: the CLOSE handler, which is what an abrupt /q or a
// killed client produces, and the silence sweep in Tick for a link that dies
// without even managing a CLOSE.
// ============================================================
void TrilogyZoneServer::EnterLinkdead(Session& s, uint64_t now_ms)
{
	if (!s.trilogy_client || s.linkdead_since_ms != 0) return;

	s.linkdead_since_ms  = now_ms;
	s.linkdead_entry_pkt = s.last_pkt;
	s.trilogy_client->SetLinkdead(true);

	// Observers show the LD tag.  The Trilogy appearance translator forwards
	// any type verbatim, so this reaches v29c as 0xf520 type 18 and other
	// client versions natively.
	auto outapp = new EQApplicationPacket(
	    OP_SpawnAppearance, sizeof(::SpawnAppearance_Struct));
	auto* sa = reinterpret_cast<::SpawnAppearance_Struct*>(outapp->pBuffer);
	sa->spawn_id  = s.trilogy_client->GetID();
	sa->type      = AppearanceType::Linkdead; // 18
	sa->parameter = 1;
	entity_list.QueueClients(s.trilogy_client, outapp, true); // ignore self
	safe_delete(outapp);

	// Hand the body to server AI for the window.  The client is gone, so the
	// input freeze AI_Start imposes costs nothing, and a mob mid-fight keeps a
	// target that can flee and die instead of one that evaporates.
	s.trilogy_client->AI_Start(static_cast<uint32>(kLinkdeadHoldMs));

	LogInfo("[TrilogyZone] Linkdead: char=[{}] holding {} ms before teardown",
	        s.char_name, kLinkdeadHoldMs);
}

void TrilogyZoneServer::SendToSession(uint64_t session_key, uint16_t opcode,
                                      const uint8_t* data, uint32_t size,
                                      bool ack_req)
{
	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	Session& s = it->second;
	if (s.state != CONNECTED) return;
	// Nothing is listening at the far end during a linkdead hold — the socket
	// peer is gone.  The session is kept only so the body stays in the world.
	if (s.linkdead_since_ms != 0) return;
	SendApp(s.source_addr, s.source_port, s, opcode, data, size, ack_req);
}

void TrilogyZoneServer::SendCloseToSession(uint64_t session_key)
{
	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	Session& s = it->second;
	SendClose(s.source_addr, s.source_port, s);
}

void TrilogyZoneServer::NoteKnownSpawn(uint64_t session_key, uint16_t spawn_id)
{
	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	it->second.known_spawns.insert(spawn_id);
}

void TrilogyZoneServer::ForgetKnownSpawn(uint64_t session_key, uint16_t spawn_id)
{
	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	it->second.known_spawns.erase(spawn_id);
	// Keep both maps in lockstep so a subsequent re-spawn (spawn_id recycled
	// by EQEmu) doesn't inherit stale broadcast state from the prior mob.
	it->second.last_broadcast.erase(spawn_id);
}

void TrilogyZoneServer::NoteKnownSpawnAt(uint64_t session_key, uint16_t spawn_id,
                                          int16_t x_pos, int16_t y_pos, int16_t z_pos,
                                          int8_t heading)
{
	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	auto& s = it->second;
	s.known_spawns.insert(spawn_id);
	auto& lb = s.last_broadcast[spawn_id];
	lb.x_pos     = x_pos;
	lb.y_pos     = y_pos;
	lb.z_pos     = z_pos;
	lb.heading   = heading;
	lb.anim_type = 0; // initial spawn / permanent-single: not-moving default
	lb.sent_ms   = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
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

	if (hdr1 & HDR1_ARSP) {
		if (o + 2 > size - 4) return;
		// Client's cumulative ack of our outbound ARQs.  Drives the
		// unacked-pending gate in SendApp (EQClassic gap-16 prevention)
		// AND pops acked entries from the EQClassic-faithful resend queue.
		uint16_t arsp = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
		if (s.sack_init) {
			int16_t delta = static_cast<int16_t>(arsp - s.acked_arq);
			if (delta > 0) {
				s.acked_arq = arsp; // monotonic, wrap-safe
				// Drop every queued ARQ that's now acknowledged.  Match
				// EQClassic IncomingARSP: `dwARSP - top->dwARQ >= 0`
				// (signed-int16 diff, wrap-safe).
				while (!s.resend_queue.empty()) {
					int16_t d = static_cast<int16_t>(arsp - s.resend_queue.front().arq);
					if (d < 0) break; // queue head still pending
					s.resend_queue.pop_front();
				}
				// Per-packet next_retry_ms means we no longer need a
				// session-wide timer to disable — empty queue = nothing to
				// retry on the next Tick.
			}
		}
		o += 2;
	}
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
			Session& cs = it->second;
			LogInfo("[TrilogyZone] CLIENT sent CLOSE from {}:{} (client-initiated disconnect)", addr, port);
			SendClose(addr, port, cs);

			// A CLOSE is not automatically a clean exit.  v29c sends one for
			// /q, for Alt-F4 and for a killed process, with no OP_Camp (0x0722)
			// beforehand — verified in the zone log, where a /q produces the
			// CLOSE line and nothing else.  Only two cases are genuinely clean:
			// a completed camp-out, which sets `camping` when 0x0722 arrives,
			// and a zone transfer, where the client closes the old zone's
			// connection on purpose.  Everything else is the link going away
			// while the character is still standing in the world, which is
			// exactly what EQClassic treats as linkdead.
			const bool clean_exit =
				cs.camping ||
				(cs.trilogy_client && cs.trilogy_client->IsZoning());

			if (clean_exit) {
				RemoveSession(key);
			} else {
				EnterLinkdead(cs, static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now().time_since_epoch()).count()));
			}
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
			existing.trilogy_client  = nullptr;
			existing.eqemu_entity_id = 0;
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
		existing.acked_arq  = 0;
		existing.last_rx_arq      = 0;
		existing.have_last_rx_arq = false;
		existing.asq_hi     = 1;
		existing.asq_lo     = 0;
		existing.ack_due    = false;
		existing.frag_groups.clear();
		existing.resend_queue.clear();
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

	// ---- ARQ retransmit handling ------------------------------------------
	//
	// v29c resends a reliable packet when it has not seen our ACK, and the
	// resend carries the SAME arq.  Two things follow.  The fragment path
	// above already does both; this is that logic applied to whole packets,
	// not a new design.
	//
	// 1. ACK before dispatching.  The ACK used to go out at the tail of
	//    OnOpcode's CONNECTED branch, or piggybacked on whatever the handler
	//    sent first — which puts handler latency inside the client's
	//    retransmit window.  A GM command doing an item-table search, or
	//    #summonitem building a 292-byte OP_SummonedItem behind a dozen
	//    hex-dump log lines, overran it: measured in a live session where
	//    three of four commands arrived twice and the one fast command did
	//    not.  Acknowledging up front takes processing time out of that loop.
	//
	//    CONNECTED only, deliberately.  Each CONNECTING state decides when to
	//    ACK as part of a handshake whose pacing is load-bearing — echoing
	//    0x9220 too late races ZoneSpawns and crashes the client — and none of
	//    those handlers is slow enough to provoke a retransmit anyway.  Not
	//    worth perturbing to fix a problem that only exists in CONNECTED.
	//
	// 2. Skip the duplicate regardless of state.  Early-ACKing makes
	//    retransmits rare, not impossible: on a real link the ACK itself gets
	//    lost and the client is right to resend.  Dropping the re-dispatch is
	//    the correctness half, and it belongs here rather than in each
	//    handler — the Trilogy handlers and the Client::Handle_* they forward
	//    into were all written assuming one delivery, and every one that
	//    mutates state on receipt is exposed.  OP_LootItem was patched
	//    individually for exactly this in #16.
	//
	//    A skipped duplicate must still be ACKed: the missing ACK is why the
	//    client resent, and in the CONNECTING states the handler we are
	//    skipping is what would otherwise have sent it.  Without that the
	//    handshake would resend forever.
	//
	// Equality only, never a range comparison, so the 16-bit wrap needs no
	// special case — a false positive would require two consecutive reliable
	// packets carrying the same arq, which is the definition of a retransmit.
	// Unreliable traffic (has_arq false: position updates and the like) is
	// never deduplicated; it carries no sequence to compare, and two genuine
	// updates in a row must both be processed.
	if (session.state == CONNECTED && session.ack_due) {
		SendAck(addr, port, session);
	}

	if (has_arq) {
		if (session.have_last_rx_arq && session.last_rx_arq == cli_arq) {
			if (session.ack_due) SendAck(addr, port, session);
			LogInfo("[TrilogyZone] rx DUPLICATE arq={:04X} opcode={:04X} plen={} state={} — "
			        "retransmit of a packet already dispatched; skipping handler",
			        cli_arq, opcode, plen, static_cast<int>(session.state));
			return;
		}
		session.last_rx_arq      = cli_arq;
		session.have_last_rx_arq = true;
	}

	OnOpcode(addr, port, session, opcode, payload, plen);
}

// ============================================================
// [TrilogyAirProbe] — dump the CLIENT-UPLOADED PlayerProfile so we can locate
//   the v29c "air / breath remaining" slot.
//
// WHY THIS EXISTS
//   Drowning in this era is client-authoritative: the v29c client owns the
//   breath meter and only tells us "I took 0xFB damage" via OP_Action (0x5820).
//   Our PlayerProfile encoder has NO air field mapped, so SendPlayerProfile
//   ships 0x00 in whatever slot the client reads its breath from — which is
//   why a player who crosses a zone line while submerged arrives with an empty
//   bar and drowns immediately, regardless of how much breath he had.
//   Modern EQEmu fixes this by stamping the field at zone-entry
//   (client_packet.cpp:1647 "Reset to max so they dont drown on zone in if its
//   underwater"), and EQMacEmu does the same for the Mac client
//   (mac_structs.h:1245 air_remaining @5900).  Neither EQClassic nor EQMacEmu
//   ever located the field for the 8104-byte Trilogy PP — EQMacEmu still lists
//   `OUT(air_remaining);` in its unresolved "Find these:" block
//   (EQMacEmuTrilogy/common/patches/trilogy.cpp:302).  We cannot port the fix
//   until we know the offset, hence this probe.
//
// HOW IT WORKS
//   The v29c client hands us its OWN full PlayerProfile on camp / NPC-trade
//   (0x2e20) and before zoning (0x5421 / 0x5521 — Harakiri: "Client sends this
//   before zoning or every couple minutes", EQClassic client_process.cpp:6862).
//   That upload is ground truth for every field the client maintains locally,
//   air included.  We ship the unknown regions as zeros, so any NON-ZERO byte
//   in them is client-owned state.  Take two samples and diff the NONZERO line:
//     A) stand on dry land, breath bar full, zone (or camp)
//     B) submerge, let the bar drain most of the way WITHOUT dying, zone
//   The air slot is the small integer that shrank between A and B.
//
// PAYLOAD FRAMING
//   0x2e20 arrives as the full 8104-byte struct.  The 0x54xx / 0x55xx saves
//   arrive checksum-stripped at 8100 bytes — EQClassic performs the same
//   rebase (`pBuffer - PLAYERPROFILE_CHECKSUM_LENGTH`) in ProcessOP_PlayerSave.
//   Both are normalised into an 8104-byte view here so every printed offset is
//   a real struct offset you can look up in trilogy_structs.h.
//
// This is read-only.  Nothing here writes to the DB or to m_pp; the uploaded
// profile is never trusted (same stance as EQClassic, which keeps only
// `drunkeness` from it).  air_supply (byte 2633) was pinned this way on
// 2026-08-27; the probe is kept because the same two lines will crack the next
// unknown PP field, and it only fires on a PP upload (camp / NPC-trade / the
// client's periodic save), not per tick.
// ============================================================

namespace {

struct PPUnknownRange {
	uint16_t    off;
	uint16_t    len;
	const char* tag;
};

// Every declared-unknown span in Trilogy::structs::PlayerProfile_Struct.
// The client-owned POINTER arrays (inventoryitemPointers @228,
// bankinvitemPointers @3912) are deliberately excluded: they are heap
// addresses that change on every sample and would bury the signal.
constexpr PPUnknownRange kPPUnknownRanges[] = {
	{   59,    1, "u59"   },
	{   61,    3, "u61"   },
	{   73,   47, "u73"   },
	{  122,    1, "u122"  },
	{  154,   14, "u154"  },
	{ 2406,    2, "u2406" },
	{ 2439,   21, "u2439" },
	{ 2582,  162, "u2582" },  // PRIME suspect — dumped in full below
	{ 2745,    3, "u2745" },
	{ 2749,   15, "u2749" },
	{ 2765,   23, "u2765" },
	{ 2789,   23, "u2789" },
	{ 2820,   24, "u2820" },
	{ 3824,    4, "u3824" },
	{ 3888,   24, "u3888" },
	{ 3944,   12, "u3944" },
	{ 3960,   20, "u3960" },
	{ 4157,    1, "u4157" },
	{ 4168,    2, "u4168" },
	{ 4171,    2, "u4171" },
	{ 4174,    1, "u4174" },
	{ 4178,    2, "u4178" },
	{ 4212,    4, "u4212" },
	{ 4508, 3592, "u4508" },
};

// Little-endian scalar read out of the normalised PP view.  memcpy rather than
// a reinterpret_cast because these offsets are deliberately unaligned.
template <typename T>
static T PPRead(const std::vector<uint8_t>& pp, size_t off)
{
	T v{};
	if (off + sizeof(T) <= pp.size()) {
		memcpy(&v, pp.data() + off, sizeof(T));
	}
	return v;
}

static void LogTrilogyAirProbe(uint32_t char_id, const char* src,
							   const uint8_t* payload, uint32_t plen)
{
	constexpr uint32_t kPPSize      = sizeof(Trilogy::structs::PlayerProfile_Struct); // 8104
	constexpr uint32_t kChecksumLen = 4;

	uint32_t base = 0;
	if (plen >= kPPSize) {
		base = 0;                 // 0x2e20: full struct including checksum
	} else if (plen >= kPPSize - kChecksumLen) {
		base = kChecksumLen;      // 0x5421 / 0x5521: checksum stripped
	} else {
		LogInfo("[TrilogyAirProbe] char={} src={} SKIP plen={} (need {} or {})",
				char_id, src, plen, kPPSize, kPPSize - kChecksumLen);
		return;
	}

	std::vector<uint8_t> pp(kPPSize, 0);
	memcpy(pp.data() + base, payload, kPPSize - base);

	// Alignment sanity check.  If name / zone / level / hunger decode to
	// nonsense then the rebase above is wrong and EVERY offset below is
	// garbage - read this line first before trusting anything else.
	char name[31]      = {0};
	char zone_name[16] = {0};
	memcpy(name,      pp.data() +    4, 30);
	memcpy(zone_name, pp.data() + 2424, 15);
	LogInfo("[TrilogyAirProbe] char={} src={} plen={} base={} | ALIGN name='{}' zone='{}'"
			" level={} hp={} hunger={} thirst={} fatigue={} drunk={}",
			char_id, src, plen, base,
			static_cast<const char*>(name), static_cast<const char*>(zone_name),
			static_cast<unsigned>(pp[60]),
			PPRead<int16_t>(pp, 120),
			PPRead<int32_t>(pp, 2812),
			PPRead<int32_t>(pp, 2816),
			static_cast<unsigned>(pp[4170]),
			static_cast<unsigned>(pp[4176]));

	// PRIME suspect window: struct 2582..2743, the gap between skills[74] and
	// autosplit.  In the PoP-era Mac PlayerProfile - the direct descendant of
	// this layout - the same gap holds innate[], a 1-byte void, air_supply
	// (uint16), texture, height/width/length/view_height, boat[32] and 60 bytes
	// of pad (mac_structs.h 3136..3348 = 212 bytes).  Widening skills[] and
	// innate[] from int8 to uint16 accounts for the size difference EXACTLY
	// (212 - 162 = 50 = the innate entry count), which lands air at ~2633 here.
	// Dumped in full so the hypothesis can be confirmed or killed by eye.
	{
		std::string hex;
		hex.reserve(162 * 3);
		for (int i = 0; i < 162; ++i) {
			hex += fmt::format("{:02X} ", pp[2582 + i]);
		}
		LogInfo("[TrilogyAirProbe] char={} src={} PRIME 2582..2743=[{}]", char_id, src, hex);
		LogInfo("[TrilogyAirProbe] char={} src={} CANDIDATE air_2633_u16={}"
				" b2632={} b2633={} b2634={} b2635={}",
				char_id, src, PPRead<uint16_t>(pp, 2633),
				static_cast<unsigned>(pp[2632]), static_cast<unsigned>(pp[2633]),
				static_cast<unsigned>(pp[2634]), static_cast<unsigned>(pp[2635]));
	}

	// Every declared-unknown span, non-zero bytes only, as offset=value pairs.
	// SendPlayerProfile ships these as 0x00, so anything non-zero is a value
	// the CLIENT put there - exactly what we are hunting.  Diff this line
	// between a full-bar sample and a low-bar sample.
	std::string pairs;
	std::string counts;
	unsigned    shown = 0;
	unsigned    total = 0;
	for (const auto& r : kPPUnknownRanges) {
		unsigned in_range = 0;
		for (uint16_t i = 0; i < r.len; ++i) {
			const uint16_t off = static_cast<uint16_t>(r.off + i);
			if (!pp[off]) {
				continue;
			}
			++in_range;
			++total;
			if (shown < 400) {   // hard cap so one bad sample cannot flood the log
				++shown;
				pairs += fmt::format("{}={:02X} ", off, pp[off]);
			}
		}
		if (in_range) {
			counts += fmt::format("{}:{} ", r.tag, in_range);
		}
	}
	if (pairs.empty())  { pairs  = "(all zero)"; }
	if (counts.empty()) { counts = "(none)"; }
	LogInfo("[TrilogyAirProbe] char={} src={} NONZERO-UNKNOWN shown={}/{} per-range=[{}] [{}]",
			char_id, src, shown, total, counts, pairs);
}

} // namespace

// ============================================================
// Opcode dispatch — state-machine gated
// ============================================================

void TrilogyZoneServer::OnOpcode(const std::string& addr, int port, Session& s,
                                  uint16_t opcode, const uint8_t* payload, uint32_t plen)
{
	LogNetcode("[TrilogyZone] rx opcode={:04X} plen={} state={} from {}:{}",
	        opcode, plen, static_cast<int>(s.state), addr, port);

	// Connect-handshake blind spot.  Every CONNECTING state below drops an
	// opcode it does not match, silently — there is no equivalent of the
	// CONNECTED-state UNHANDLED logger, so anything the client says during
	// zone-in has never been visible.  That is the same failure the handbook
	// records for 0xff21: scored as handled because nothing complained.
	// Log it once per (state, opcode) pair so a chatty handshake cannot flood.
	const auto note_unhandled = [&](const char* state_name) {
		const uint32_t key = (static_cast<uint32_t>(s.state) << 16) | opcode;
		if (!s.connect_unhandled_seen.insert(key).second) return;
		std::string hex;
		const uint32_t cap = plen > 64 ? 64u : plen;
		for (uint32_t i = 0; i < cap; ++i) hex += fmt::format("{:02X} ", payload[i]);
		if (plen > cap) hex += "...";
		LogInfo("[TrilogyZone] UNHANDLED rx opcode={:04X} state={} plen={} payload=[{}]",
		        opcode, state_name, plen, hex);
	};

	switch (s.state) {
	case CONNECTING1:
		if (opcode == ZN_OP_SetDataRate)
			HandleSetDataRate(addr, port, s);
		else {
			note_unhandled("CONNECTING1");
			if (s.ack_due) SendAck(addr, port, s);
		}
		break;

	case CONNECTING2:
		if (opcode == ZN_OP_ZoneEntry)
			HandleZoneEntry(addr, port, s, payload, plen);
		else {
			note_unhandled("CONNECTING2");
			if (s.ack_due) SendAck(addr, port, s);
		}
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
		else {
			note_unhandled("CONNECTING3");
			if (s.ack_due) SendAck(addr, port, s);
		}
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
		else {
			note_unhandled("CONNECTING4");
			if (s.ack_due) SendAck(addr, port, s);
		}
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
		else {
			note_unhandled("CONNECTING5");
			if (s.ack_due) SendAck(addr, port, s);
		}
		break;

	case CONNECTED:
		// TrilogyZonePointDebug diagnostic — log EVERY rx opcode in CONNECTED
		// state (opcode, plen, first 32 bytes of payload). Purpose: when the
		// v29c client crosses a Skyshrine teleporter pad, we don't yet know
		// which packet (if any) it emits, and the normal handler-specific
		// logging doesn't capture handled opcodes in one place. Turn this on
		// with `#rules set Zone TrilogyZonePointDebug 1`, walk onto a pad,
		// turn back off. Compare the rx trace against a normal walk to
		// isolate the pad-crossing opcode.
		if (RuleB(Zone, TrilogyZonePointDebug)) {
			std::string hex_dbg;
			const uint32_t cap_dbg = plen > 32 ? 32u : plen;
			for (uint32_t i = 0; i < cap_dbg; ++i) {
				hex_dbg += fmt::format("{:02X} ", payload[i]);
			}
			if (plen > cap_dbg) hex_dbg += "...";
			LogInfo("[TrilogyZP DBG] rx opcode={:04X} plen={} payload=[{}]",
			        opcode, plen, hex_dbg);
		}
		if (opcode == ZN_OP_ClientUpdate)
			HandleClientUpdate(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ChannelMsg)
			HandleChannelMessage(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_MoveItem) {
			// Snapshot from_slot BEFORE HandleMoveItem runs — if it was 0 the client
			// just emptied its cursor (drop, destroy, equip, place-in-bag).  We use
			// that as the trigger to dispatch any queued OP_SummonedItem (deferred
			// cursor deliveries from rapid multi-loot / #si / spells) — matches
			// EQClassic's client_process.cpp:1774-1779 "pop summonedItems when
			// pp.inventory[0] == 0xFFFF" post-move check.  See
			// TrilogyClient::OnClientCursorCleared for full rationale.
			uint32_t cursor_clear_probe = 0xFFFFFFFFu;
			if (plen >= sizeof(Trilogy::structs::MoveItem_Struct)) {
				cursor_clear_probe =
				    reinterpret_cast<const Trilogy::structs::MoveItem_Struct*>(payload)->from_slot;
			}
			HandleMoveItem(addr, port, s, payload, plen);
			if (cursor_clear_probe == 0 && s.trilogy_client) {
				s.trilogy_client->OnClientCursorCleared();
			}
		}
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

			// Drop-to-ground empties the client cursor — dispatch any deferred
			// OP_SummonedItem waiting on this exact event.  See
			// TrilogyClient::OnClientCursorCleared.
			s.trilogy_client->OnClientCursorCleared();
		}
		else if (opcode == ZN_OP_PickupItem && s.trilogy_client)
		{
			// Player clicked a ground item to pick it up.
			// payload = ClickObject_Struct: objectID(uint32) + playerID(uint32)
			if (plen >= sizeof(ClickObject_Struct)) {
				const auto* co_in = reinterpret_cast<const ClickObject_Struct*>(payload);
				Entity* ent = entity_list.GetID(static_cast<uint16>(co_in->drop_id));
				LogInfo("[TRILOGY-TS] OP_PickupItem incoming drop_id={} player_id={} ent_found={} is_object={} obj_type={}",
				        co_in->drop_id, co_in->player_id,
				        ent ? 1 : 0,
				        (ent && ent->IsObject()) ? 1 : 0,
				        (ent && ent->IsObject()) ? (int)ent->CastToObject()->GetType() : -1);
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
				const uint8_t doorid = payload[0];
				// Diagnostic: log every door click so we can confirm what the v29c
				// client sends.  Kept after the elevator fix to verify routing.
				Doors* d = entity_list.FindDoor(doorid);
				if (d) {
					LogInfo("[TrilogyClient] OP_ClickDoor zone=[{}] doorid=[{}] "
					        "name=[{}] opentype=[{}] triggerdoor=[{}] triggertype=[{}] "
					        "door_param=[{}] invert=[{}] is_open=[{}] plen=[{}]",
					        zone ? zone->GetShortName() : "?", doorid,
					        d->GetDoorName(), d->GetOpenType(),
					        d->GetTriggerDoorID(), d->GetTriggerType(),
					        d->GetDoorParam(), d->GetInvertState(),
					        d->IsDoorOpen() ? 1 : 0, plen);
				} else {
					LogInfo("[TrilogyClient] OP_ClickDoor zone=[{}] doorid=[{}] "
					        "(no matching Doors entity) plen=[{}]",
					        zone ? zone->GetShortName() : "?", doorid, plen);
				}

				// === Kelethin elevator special case ====================================
				// FELE2 = elevator button, FAYLEVATOR = elevator platform (gfaydark).
				// EQEmu's generic HandleClick path is wrong for these:
				//   (1) it emits OP_MoveDoor for the BUTTON itself, which v29c has no
				//       animation for — wasted packet that adds noise, and
				//   (2) it relies on a 5-second close timer to reset m_is_open, but for
				//       opentype 59 the timer fires silently (no close packet to the
				//       client; see Doors::Process), so after timeout the server thinks
				//       the door is closed but the client doesn't — next click sends
				//       OPEN again and the platform never goes back down.
				//
				// EQClassic's working implementation (Zone/Source/client_process.cpp
				// ProcessOP_ClickDoor, ~L4869) instead sends ONE OP_OpenDoor for the
				// TRIGGER (platform) only, with a per-click alternating action.  Mirror
				// that: skip the button, toggle the platform's m_is_open directly (no
				// timer involved), send a single 0x8E20 with the alternating action.
				static constexpr uint8 kOpenDoor   = 0x02;
				static constexpr uint8 kCloseDoor  = 0x03;
				const bool is_fele_button =
					d &&
					d->GetTriggerDoorID() != 0 &&
					d->GetOpenType() == 59 &&
					strncasecmp(d->GetDoorName(), "FELE", 4) == 0;

				if (is_fele_button) {
					Doors* platform = entity_list.FindDoor(d->GetTriggerDoorID());
					if (platform) {
						const bool was_open = platform->IsDoorOpen();
						// SendDoorSpawns forces elevators to spawn with
						// inverted=0/doorIsOpen=0 on the client (see the
						// is_elevator guard there), so the v29c client
						// tracks the platform as a non-inverted door.
						// Mirror that here: ignore the DB invert_state
						// and toggle OPEN/CLOSE straight off was_open.
						// With the DB invert dance, click 1 would send
						// CLOSE to a client that already thinks the
						// platform is closed — a no-op, so the platform
						// wouldn't move until click 2.  Plain toggle keeps
						// both sides in sync from the first click on.
						const uint8 action = was_open ? kCloseDoor : kOpenDoor;
						platform->SetOpenState(!was_open);

						// (1) Drive the button's own "depress" animation.  v29c only
						// animates a door when it receives OP_MoveDoor (0x8E20) AND
						// the action represents a state change from what the client
						// tracks locally.  If we always send OPEN, the first click
						// animates (CLOSED→OPEN) but every subsequent click is a
						// no-op because the client thinks the button is already
						// OPEN — door_param=1 only auto-pops it visually, the
						// client's tracked state still says OPEN.
						//
						// Fix: alternate the button's action in lock-step with the
						// platform — OPEN when sending the platform out, CLOSE
						// when calling it back.  The client sees a real state
						// toggle on every click and re-animates each time,
						// giving press-feedback on every interaction.  Mirror
						// the same state on the server-side Doors entity so
						// EQEmu's view stays in sync with what we told the client.
						const uint8 btn_action = d->IsDoorOpen() ? kCloseDoor : kOpenDoor;
						d->SetOpenState(!d->IsDoorOpen());

						auto* btn_app = new EQApplicationPacket(OP_MoveDoor, sizeof(::MoveDoor_Struct));
						auto* btn_md  = reinterpret_cast<::MoveDoor_Struct*>(btn_app->pBuffer);
						btn_md->doorid = static_cast<uint8>(d->GetDoorID());
						btn_md->action = btn_action;
						entity_list.QueueClients(s.trilogy_client, btn_app, false);
						safe_delete(btn_app);

						// (2) Drive the platform.  QueueClients(sender, app,
						// ignore_sender=false) reaches every Client in the zone
						// including the clicker — TrilogyClients see it via the
						// QueuePacket override that translates OP_MoveDoor into
						// 0x8E20 (HandleMoveDoor in trilogy_client.cpp).
						auto* outapp = new EQApplicationPacket(OP_MoveDoor, sizeof(::MoveDoor_Struct));
						auto* md = reinterpret_cast<::MoveDoor_Struct*>(outapp->pBuffer);
						md->doorid = static_cast<uint8>(platform->GetDoorID());
						md->action = action;
						entity_list.QueueClients(s.trilogy_client, outapp, false);
						safe_delete(outapp);

						LogInfo("[TrilogyClient] Elevator click: button=[{}] "
						        "btn_action=[{:#x}] btn_now_open=[{}] "
						        "platform=[{}] platform_action=[{:#x}] "
						        "was_open=[{}] -> now=[{}]",
						        d->GetDoorID(), btn_action, d->IsDoorOpen() ? 1 : 0,
						        platform->GetDoorID(), action,
						        was_open ? 1 : 0, was_open ? 0 : 1);
						break;
					}
					// Trigger door missing — fall through to generic path as a fallback.
				}
				// === end Kelethin elevator special case ================================

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
		// 0xe620 is also sent by the receiving client to ACCEPT a PC-trade request
		// (NPC trades echo it from server→client; the inbound direction here is the
		// recipient agreeing to open their window — relayed to the requester).
		else if (opcode == ZN_OP_TradeAccept && s.trilogy_client)
			HandleTradeAccepted(addr, port, s, payload, plen);
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
		else if (opcode == ZN_OP_TradeSkillCombine && s.trilogy_client)
		{
			// Player hit the Combine button in the tradeskill UI (world container OR
			// inventory bag).  Trilogy Combine_Struct (32B) carries the container
			// itself + a snapshot of the 10 ingredient item IDs, but
			// Object::HandleCombine actually reads ingredients live from the
			// container's bag — so we only need to translate the container_slot.
			//
			// Wire containerslot semantics (per EQClassic + Combine_Struct comment):
			//   1000      = world container (m_tradeskill_object set on click)
			//   22-29     = personal inventory bag slot 1-8
			//   2000-2007 = bank top slot (combine-in-bank is rare; pass through)
			// Trilogy v29c wire ↔ EQEmu DB slot mapping (mirrors HandleMoveItem):
			//   wire 21-29 → DB 22-30 (+1 shift, EQEmu has a charm slot v29c lacks).
			// Object::HandleCombine treats container_slot == 1000 specially and
			// uses m_tradeskill_object; otherwise it does user_inv.GetItem(slot),
			// which expects the EQEmu/RoF2-style DB slot.
			if (plen >= sizeof(Trilogy::structs::TradeSkillCombine_Struct)) {
				const auto* in = reinterpret_cast<const Trilogy::structs::TradeSkillCombine_Struct*>(payload);

				int16_t emu_slot;
				if (in->containerslot == 1000) {
					emu_slot = static_cast<int16_t>(EQ::invslot::SLOT_TRADESKILL_EXPERIMENT_COMBINE);
				} else if (in->containerslot >= 21 && in->containerslot <= 29) {
					emu_slot = static_cast<int16_t>(in->containerslot + 1);
				} else {
					emu_slot = static_cast<int16_t>(in->containerslot);
				}

				LogInfo("[TrilogyZone] OP_TradeSkillCombine char={} wire_container={} emu_container={} "
				        "objtype={} containerID={}",
				        s.char_id, in->containerslot, emu_slot,
				        in->worldobjecttype, in->containerID);

				// Diagnostic: confirm m_inv state at the combine container slot so we
				// can tell apart "no recipe in DB" from "m_inv stale".  GetTradeRecipe
				// silently returns false for both cases; the player sees nothing.
				if (emu_slot != 1000) {
					auto& dbg_inv = s.trilogy_client->GetInv();
					const EQ::ItemInstance* dbg_bag = dbg_inv.GetItem(emu_slot);
					if (!dbg_bag) {
						LogInfo("[TrilogyZone] OP_TradeSkillCombine DIAG: m_inv[{}] is EMPTY — "
						        "bag not loaded; combine will fail silently", emu_slot);
					} else {
						const EQ::ItemData* dbg_item = dbg_bag->GetItem();
						int          dbg_n   = 0;
						uint32_t     dbg_sum = 0;
						std::string  dbg_ids;
						for (uint8 i = 0; i < 10; i++) {
							const auto* ci = dbg_bag->GetItem(i);
							if (!ci) continue;
							const auto* cit = ci->GetItem();
							if (!cit) continue;
							dbg_n++;
							dbg_sum += cit->ID;
							if (!dbg_ids.empty()) dbg_ids += ",";
							dbg_ids += std::to_string(cit->ID);
						}
						LogInfo("[TrilogyZone] OP_TradeSkillCombine DIAG: m_inv[{}] container item_id={} "
						        "is_bag={} bag_type={} contents={}/10 ids=[{}] sum={}",
						        emu_slot,
						        dbg_item ? dbg_item->ID : 0,
						        dbg_bag->IsType(EQ::item::ItemClassBag) ? 1 : 0,
						        dbg_item ? (int)dbg_item->BagType : -1,
						        dbg_n, dbg_ids, dbg_sum);
						LogInfo("[TrilogyZone] OP_TradeSkillCombine DIAG: GetTradeRecipe will query: "
						        "ingredients IN ({}) AND container IN ({}, {}) HAVING count={} sum={} -- "
						        "check DB: SELECT * FROM tradeskill_recipe_entries WHERE recipe_id IN "
						        "(SELECT id FROM tradeskill_recipe WHERE enabled=1) AND "
						        "((item_id IN ({}) AND componentcount>0) OR (item_id IN ({}, {}) AND iscontainer=1))",
						        dbg_ids,
						        dbg_item ? (int)dbg_item->BagType : -1,
						        dbg_item ? dbg_item->ID : 0,
						        dbg_n, dbg_sum,
						        dbg_ids,
						        dbg_item ? (int)dbg_item->BagType : -1,
						        dbg_item ? dbg_item->ID : 0);
					}
				}

				EQApplicationPacket pkt(OP_TradeSkillCombine, sizeof(NewCombine_Struct));
				auto* nc = reinterpret_cast<NewCombine_Struct*>(pkt.pBuffer);
				nc->container_slot     = emu_slot;
				nc->guildtribute_slot  = 0;
				s.trilogy_client->Handle_OP_TradeSkillCombine(&pkt);

				// Echo the original 32-byte Combine_Struct back as the wire ack.
				// EQClassic's ProcessOP_TradeSkillCombine re-queues `pApp` verbatim
				// at the end of the handler (Zone/Source/Tradeskills.cpp:218); the
				// v29c client treats this as the "combine processed" confirmation
				// and ignores 0-byte versions.  Sending it AFTER Handle_OP_TradeSkillCombine
				// keeps the ordering right (cursor result + container clear arrive
				// first via the standard outgoing path, ack last).
				SendApp(addr, port, s, ZN_OP_TradeSkillCombine,
				        payload, sizeof(Trilogy::structs::TradeSkillCombine_Struct));
			}
		}
		else if (opcode == ZN_OP_CraftingStation && s.trilogy_client)
		{
			// Player closed the tradeskill container UI (v29c sends the same 20-byte
			// ClickObjectAck_Struct back with open=0).  Match EQClassic's
			// ProcessOP_CraftingStation semantics: persist the m_inst contents
			// back to DB and release the server-side "in use" reference.
			// Items remain in the container so the next player who clicks it
			// sees them — exactly like the real Verant behaviour.
			//
			// Deliberately bypass Client::Handle_OP_ClickObjectAction here.  That
			// path calls Object::Close() which dumps any leftover items back into
			// the player's m_inv via MoveItemToInventory — and m_inv is stale for
			// Trilogy sessions (we write inventory directly to the DB), so those
			// items would land in memory but never reach the inventory table.
			Object* obj = s.trilogy_client->GetTradeskillObject();
			if (obj) {
				LogInfo("[TrilogyZone] OP_CraftingStation (close) char={} obj_id={}",
				        s.char_id, obj->GetID());
				obj->Save();         // m_inst → DB
				obj->ReleaseUser();  // clear "in use" + SetTradeskillObject(nullptr)
			} else {
				LogInfo("[TrilogyZone] OP_CraftingStation (close) char={} but no open tradeskill object",
				        s.char_id);
			}
		}
		else if (opcode == ZN_OP_GroupInvite && s.trilogy_client) {
			s.trilogy_client->HandleIncomingGroupInvite(payload, plen);
		}
		else if (opcode == ZN_OP_GroupInvite2 && s.trilogy_client) {
			s.trilogy_client->HandleIncomingGroupInvite2(payload, plen);
		}
		else if (opcode == ZN_OP_GroupFollow && s.trilogy_client) {
			s.trilogy_client->HandleIncomingGroupFollow(payload, plen);
		}
		else if (opcode == ZN_OP_GroupCancelInvite && s.trilogy_client) {
			s.trilogy_client->HandleIncomingGroupCancelInvite(payload, plen);
		}
		else if (opcode == ZN_OP_GroupDisband && s.trilogy_client) {
			s.trilogy_client->HandleIncomingGroupDisband(payload, plen);
		}
		else if (opcode == ZN_OP_InspectRequest && s.trilogy_client)
			HandleInspectRequest(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_InspectAnswer && s.trilogy_client)
			HandleInspectAnswer(addr, port, s, payload, plen);
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
		else if (opcode == ZN_OP_Surname && s.trilogy_client)
			HandleSurname(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_SocialText && s.trilogy_client)
			HandleSocialText(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_SocialAction && s.trilogy_client)
			HandleSocialAction(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_RezzAnswer && s.trilogy_client)
			HandleRezzAnswer(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_RezzRequest && s.trilogy_client) {
			// The v29c client sends this when it lands a rez spell on a corpse.
			// Modern EQEmu's SE_Revive spell-effect path already fires
			// Corpse::CastRezz from SpellFinished when the caster's spell
			// completes on a corpse target, so processing this inbound would
			// double-fire the popup on the corpse owner.  Silently drop.
		}
		else if (opcode == ZN_OP_ZoneChange && s.trilogy_client)
			HandleZoneChange(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_Buff && s.trilogy_client)
			HandleBuffCancel(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_SetServerFilter && s.trilogy_client)
			HandleServerFilter(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_WhoAll && s.trilogy_client)
			HandleWhoAll(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ZoneEntryResend && s.trilogy_client)
			HandleZoneEntryResend(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_SetRunMode && s.trilogy_client)
			HandleSetRunMode(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ClientError && s.trilogy_client)
			HandleClientError(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_PetitionRefresh) {
			// Nothing to answer with — see the constant.  Consumed so it stops
			// showing up as unhandled and hiding real gaps in the log.
		}
		else if (s.trilogy_client &&
		         (opcode == ZN_OP_Assist     || opcode == ZN_OP_Random ||
		          opcode == ZN_OP_SplitMoney || opcode == ZN_OP_Yell   ||
		          opcode == ZN_OP_LFG        || opcode == ZN_OP_ConsentRequest))
		{
			HandleSocialCommand(addr, port, s, opcode, payload, plen);
		}
		else if (s.trilogy_client &&
		         (opcode == ZN_OP_GuildInvite  || opcode == ZN_OP_GuildInviteAccept ||
		          opcode == ZN_OP_GuildRemove  || opcode == ZN_OP_GuildMOTD ||
		          opcode == ZN_OP_GuildLeader  || opcode == ZN_OP_GuildDelete ||
		          opcode == ZN_OP_GuildWar     || opcode == ZN_OP_GuildPeace ||
		          opcode == ZN_OP_GetGuildsList))
		{
			HandleGuildCommand(addr, port, s, opcode, payload, plen);
		}
		else if (opcode == ZN_OP_CastSpell && s.trilogy_client)
			HandleCastSpell(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_MemorizeSpell && s.trilogy_client)
			HandleMemorizeSpell(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_Camp && s.trilogy_client && !s.camping) {
			// Diagnostic only — the client owns the 30 s countdown and
			// signals completion via ZN_OP_DeleteSpawn (0x5021).  See the
			// long comment in Tick() for why this is not a server-driven
			// timer any more.
			s.camping    = true;
			s.camp_start = std::time(nullptr);
			LogInfo("[TrilogyZone] Camp initiated for {}", s.char_name);
		}
		else if (opcode == ZN_OP_DeleteSpawn && s.trilogy_client) {
			// Client's own 30 s /camp countdown expired without interruption.
			// This is the authoritative camp-complete signal — process the
			// full camp-out flow and drop the session immediately.  We do NOT
			// touch `s` after RemoveSession (it invalidates the reference).
			uint64_t key = SessionKey(addr, port);
			CompleteCamp(key, s);
			RemoveSession(key);
			return;
		}
		else if ((opcode == ZN_OP_PlayerSave || opcode == ZN_OP_PlayerSave2) &&
		         s.trilogy_client) {
			// Fragmented PlayerProfile dump the client sends alongside
			// OP_DeleteSpawn (and on zone-out).  EQClassic just Save()s
			// and sends no ack — the client does not wait for a reply.
			// Content of the packet is not trusted; our DB save from
			// TrilogyClient::Save() is authoritative.
			//
			// It IS read, though: this upload is the client's own view of its
			// PlayerProfile and therefore the only ground truth we have for the
			// fields v29c maintains locally — see [TrilogyAirProbe] above for
			// why we care (breath / drowning on zone-in).
			LogTrilogyAirProbe(s.char_id,
			                   opcode == ZN_OP_PlayerSave ? "0x5421" : "0x5521",
			                   payload, plen);
			if (opcode == ZN_OP_PlayerSave2) {
				s.trilogy_client->Save();
			}
		}
		else if ((opcode == ZN_OP_AutoAttack || opcode == ZN_OP_AutoAttack2) && s.trilogy_client) {
			// 4-byte payload: pBuffer[0] = 0 (off) or 1 (on).
			// Directly construct and queue a 4-byte OP_AutoAttack packet for Client::Handle_OP_AutoAttack.
			if (plen >= 1) {
				// Attack-diagnostic: capture target state at the moment the
				// user toggled auto-attack on. Answers "why does attack do
				// nothing?" — is target gone / out of range / not attackable.
				if (payload[0] == 1) {
					Mob* tgt = s.trilogy_client->GetTarget();
					if (tgt) {
						float dx = tgt->GetX() - s.trilogy_client->GetX();
						float dy = tgt->GetY() - s.trilogy_client->GetY();
						float dz = tgt->GetZ() - s.trilogy_client->GetZ();
						float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
						LogInfo("[Trilogy attack-diag] AUTO_ON tgt_sid={} name='{}' "
						        "tgt_pos=({:.1f},{:.1f},{:.1f}) player_pos=({:.1f},{:.1f},{:.1f}) "
						        "dist={:.1f} is_npc={} is_client={} is_corpse={} moving={} "
						        "hp={}/{}",
						        static_cast<int>(tgt->GetID()),
						        tgt->GetCleanName() ? tgt->GetCleanName() : "?",
						        tgt->GetX(), tgt->GetY(), tgt->GetZ(),
						        s.trilogy_client->GetX(), s.trilogy_client->GetY(),
						        s.trilogy_client->GetZ(),
						        dist,
						        tgt->IsNPC() ? 1 : 0,
						        tgt->IsClient() ? 1 : 0,
						        tgt->IsCorpse() ? 1 : 0,
						        tgt->IsMoving() ? 1 : 0,
						        tgt->GetHP(), tgt->GetMaxHP());
					} else {
						LogInfo("[Trilogy attack-diag] AUTO_ON tgt=NULL "
						        "player_pos=({:.1f},{:.1f},{:.1f})",
						        s.trilogy_client->GetX(), s.trilogy_client->GetY(),
						        s.trilogy_client->GetZ());
					}
				} else {
					LogInfo("[Trilogy attack-diag] AUTO_OFF");
				}

				EQApplicationPacket atkpkt(OP_AutoAttack, 4);
				memset(atkpkt.pBuffer, 0, 4);
				atkpkt.pBuffer[0] = payload[0];
				s.trilogy_client->Handle_OP_AutoAttack(&atkpkt);
			}
		}
		else if ((opcode == ZN_OP_ClientTarget || opcode == ZN_OP_TargetByName) &&
		         s.trilogy_client) {
			// 0xfe21 is /target <name>: the CLIENT resolves the name and sends the
			// resulting entity id, so the payload is the same ClientTarget_Struct
			// that a mouse click sends on 0x6221.  Share the path — it already
			// carries the drift-refresh and the TargetMouse-not-TargetCommand rule
			// documented below, both of which /target needs just as much.
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

				const uint64_t now_ms_tgt = static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now().time_since_epoch()).count());
				Mob* tgt_mob = (tgt32 != 0) ? entity_list.GetMob(static_cast<uint16_t>(tgt32)) : nullptr;

				// Compute client render drift for the target (regardless of
				// log rate limit) so the JIT refresh below can act on every
				// selection that needs it.
				int client_last_x = 0, client_last_y = 0;
				int client_gap    = -1;
				auto lb_it_tgt = (tgt_mob != nullptr)
				                 ? s.last_broadcast.find(static_cast<uint16_t>(tgt32))
				                 : s.last_broadcast.end();
				int cur_wire_x_tgt = 0, cur_wire_y_tgt = 0;
				if (tgt_mob) {
					cur_wire_x_tgt = static_cast<int16_t>(tgt_mob->GetX());
					cur_wire_y_tgt = static_cast<int16_t>(tgt_mob->GetY());
				}
				if (lb_it_tgt != s.last_broadcast.end()) {
					client_last_x = lb_it_tgt->second.x_pos;
					client_last_y = lb_it_tgt->second.y_pos;
					const int gx = cur_wire_x_tgt - client_last_x;
					const int gy = cur_wire_y_tgt - client_last_y;
					client_gap = static_cast<int>(std::sqrt(
						static_cast<float>(gx * gx + gy * gy)));
				}

				// ── Just-in-time (JIT) drift refresh on target selection ──
				// If the client's rendered position for this target is off
				// from the server's by more than the range-check margin, the
				// client's local range gate will reject the user's next action
				// (loot on a corpse ~40u drift, attack on a mob ~15u drift).
				// Push a fresh A120 so the client's next click has an accurate
				// position for its own range test.  Wire cost: 1 A120 per
				// target selection that actually needed correction — bounded
				// by user click rate.  Naturally throttled: back-to-back
				// clicks on the same target only fire once (last_broadcast is
				// then current, so the next click sees gap=0 and skips).
				static constexpr int kJitRefreshThresholdUnits = 20;
				if (tgt_mob && lb_it_tgt != s.last_broadcast.end() &&
				    client_gap >= kJitRefreshThresholdUnits) {
					Trilogy::structs::SpawnPositionUpdate_Struct upd{};
					upd.spawn_id = static_cast<int16_t>(tgt32);
					upd.heading  = static_cast<int8_t>(
						static_cast<uint8_t>(tgt_mob->GetHeading() / 2.0f));
					upd.y_pos    = static_cast<int16_t>(tgt_mob->GetY());
					upd.x_pos    = static_cast<int16_t>(tgt_mob->GetX());
					upd.z_pos    = static_cast<int16_t>(tgt_mob->GetZ() * 10.0f);
					if (tgt_mob->IsMoving()) {
						const int eqemu_speed = tgt_mob->IsEngaged()
						                            ? tgt_mob->GetRunspeed()
						                            : tgt_mob->GetWalkspeed();
						upd.anim_type = EncodeTrilogyAnim(tgt_mob, eqemu_speed);
					}

					uint8_t buf[4 + sizeof(upd)];
					int32_t n = 1;
					memcpy(buf, &n, 4);
					memcpy(buf + 4, &upd, sizeof(upd));
					SendApp(addr, port, s, ZN_OP_MobUpdate,
					        buf, static_cast<uint32_t>(sizeof(buf)),
					        /*ack_req=*/false);

					lb_it_tgt->second.x_pos     = upd.x_pos;
					lb_it_tgt->second.y_pos     = upd.y_pos;
					lb_it_tgt->second.z_pos     = upd.z_pos;
					lb_it_tgt->second.heading   = upd.heading;
					lb_it_tgt->second.anim_type = upd.anim_type;
					lb_it_tgt->second.sent_ms   = now_ms_tgt;

					LogInfo("[Trilogy jit-refresh] sid={} name='{}' "
					        "gap={} client_last=({},{}) → server=({:.1f},{:.1f})",
					        static_cast<int>(tgt32),
					        tgt_mob->GetCleanName() ? tgt_mob->GetCleanName() : "?",
					        client_gap, client_last_x, client_last_y,
					        tgt_mob->GetX(), tgt_mob->GetY());
				}

				// Attack-diagnostic log — rate-limited to 1s per session so
				// click-spamming doesn't flood the log.
				if (now_ms_tgt - s.last_target_log_ms >= 1000) {
					s.last_target_log_ms = now_ms_tgt;
					if (tgt_mob) {
						float dx = tgt_mob->GetX() - s.trilogy_client->GetX();
						float dy = tgt_mob->GetY() - s.trilogy_client->GetY();
						float dist = std::sqrt(dx*dx + dy*dy);
						LogInfo("[Trilogy attack-diag] TARGET wire_id={} sid={} name='{}' "
						        "server_pos=({:.1f},{:.1f},{:.1f}) player_pos=({:.1f},{:.1f},{:.1f}) "
						        "dist={:.1f} client_last=({},{}) client_gap={} "
						        "is_npc={} is_corpse={} moving={} hp={}/{}",
						        static_cast<int>(tgt16),
						        static_cast<int>(tgt_mob->GetID()),
						        tgt_mob->GetCleanName() ? tgt_mob->GetCleanName() : "?",
						        tgt_mob->GetX(), tgt_mob->GetY(), tgt_mob->GetZ(),
						        s.trilogy_client->GetX(), s.trilogy_client->GetY(),
						        s.trilogy_client->GetZ(),
						        dist,
						        client_last_x, client_last_y, client_gap,
						        tgt_mob->IsNPC() ? 1 : 0,
						        tgt_mob->IsCorpse() ? 1 : 0,
						        tgt_mob->IsMoving() ? 1 : 0,
						        tgt_mob->GetHP(), tgt_mob->GetMaxHP());
					} else if (tgt32 != 0) {
						LogInfo("[Trilogy attack-diag] TARGET wire_id={} → GONE (entity_list has no mob for this sid)",
						        static_cast<int>(tgt16));
					}
				}

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
				const uint32_t wire_slot = cs.slot;
				const int emu_slot =
				    TrilogyWireSlotToEmuSlot(cs.slot, s.cursor_from_db);
				if (emu_slot >= 0) {
					// Lazy m_inv sync from DB.  Trilogy direct-DB paths
					// (HandleShopPlayerBuy, NPC trade returns, loot transfers)
					// write to the `inventory` table without touching m_inv,
					// so Handle_OP_Consume's GetInv().GetItem(slot) returns
					// nullptr for items bought this session and bails with
					// LogError.  The v29c client visibly decrements its local
					// stack but the DB row stays at the original count — the
					// stack "reappears" full on the next zone-in because that
					// zone's SendInventoryItems reloads m_inv from the un-mutated
					// DB.  Cheap single-row lookup keeps the engine handler honest.
					if (s.trilogy_client->GetInv().GetItem(
					        static_cast<int16>(emu_slot)) == nullptr) {
						auto r = database.QueryDatabase(fmt::format(
						    "SELECT `itemid`, `charges` FROM `inventory` "
						    "WHERE `charid`={} AND `slotid`={}",
						    s.char_id, emu_slot));
						if (r.Success() && r.RowCount() > 0) {
							auto row = r.begin();
							const uint32 item_id =
							    static_cast<uint32>(Strings::ToUnsignedInt(row[0]));
							const int16 charges =
							    static_cast<int16>(Strings::ToInt(row[1]));
							if (item_id != 0) {
								EQ::ItemInstance* inst =
								    database.CreateItem(item_id, charges);
								if (inst) {
									s.trilogy_client->GetInv().PutItem(
									    static_cast<int16>(emu_slot), *inst);
									safe_delete(inst);
									LogFood("[TrilogyZone] Consume: synced m_inv from DB "
									        "slot={} item={} charges={} (was stale post-buy)",
									        emu_slot, item_id, charges);
								}
							}
						}
					}

					cs.slot = static_cast<uint32_t>(emu_slot);
					LogFood("[TrilogyZone] Consume bridge: char={} wire_slot={} "
					        "emu_slot={} type={} auto={} hunger={} thirst={}",
					        s.char_name, wire_slot, emu_slot, (int)cs.type,
					        cs.auto_consumed == 0xffffffff,
					        s.trilogy_client->GetPP().hunger_level,
					        s.trilogy_client->GetPP().thirst_level);
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
				// Fresh loot session — reset the wire slot map + retransmit dedup.
				// MakeLootRequestPackets will re-assign wire slots 1..N via
				// HandleItemPacket ItemPacketLoot's AssignLootWireSlot.
				s.trilogy_client->ResetLootSession();
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
			// Wire slot is a 1-based counter (per EQClassic wire spec + our
			// AssignLootWireSlot in HandleItemPacket).  Convert back to the EQEmu
			// corpse slot using the per-session wire→emu map that was populated when
			// MakeLootRequestPackets was translated outbound.
			if (plen >= 16) {
				EQApplicationPacket lootitempkt(OP_LootItem, 16);
				memcpy(lootitempkt.pBuffer, payload, 16);
				auto* li = reinterpret_cast<Trilogy::structs::LootingItem_Struct*>(lootitempkt.pBuffer);
				const int16_t raw_slot = li->slot_id;
				const int32_t raw_lootee = li->lootee;
				const int32_t raw_looter = li->looter;
				const int32_t raw_auto = li->auto_loot;

				// Retransmit dedup — v29c ARQ resends OP_LootItem when it thinks
				// the server didn't ACK in time; the second delivery trips
				// Corpse::LootCorpseItem's 10ms cooldown and ResetLooter() breaks
				// subsequent loots until the corpse is closed and reopened.  See
				// TrilogyClient::IsDuplicateLootItem.
				if (s.trilogy_client->IsDuplicateLootItem(
				        static_cast<uint32_t>(raw_lootee), raw_slot)) {
					break;
				}

				// Translate wire slot → EQEmu corpse slot via the session map.
				const int16_t emu_slot = s.trilogy_client->LookupLootEmuSlot(raw_slot);
				if (emu_slot < 0) {
					LogInfo("[TRILOGY-LOOT] ZN_OP_LootItem: unknown wire_slot={} "
					        "(never assigned or corpse re-opened) — dropping to avoid "
					        "spurious LootCorpseItem miss + ResetLooter cascade",
					        raw_slot);
					break;
				}
				li->slot_id = emu_slot;

				// If the corpse item at this slot is a bag with content, promote
				// auto_loot to 1 so PutLootInInventory targets a general inventory
				// slot (bag content follows via CalcSlotId(general_X, i) → DB
				// 251+(X-23)*10+i → wire 250+... which the client parses correctly).
				// Without this, right-click of a filled bag placed it on cursor →
				// contents went to cursor bag (DB 351+) → HandleMoveItem has no
				// cursor-bag content migration, so the bag arrived empty in
				// inventory after the player moved it off cursor.  This mirrors
				// EQClassic behaviour for the failure mode: EQClassic's server
				// treats bags on cursor consistently because its cursor-bag DB
				// storage matches its wire format 1:1, which is not true for us.
				// Non-bag items and empty bags still honour the client's chosen
				// auto_loot value (0=cursor, 1=auto-inventory).
				if (raw_auto == 0 && raw_lootee > 0) {
					Entity* corpse_ent = entity_list.GetID(static_cast<uint16>(raw_lootee));
					if (corpse_ent && corpse_ent->IsCorpse()) {
						Corpse* cp = corpse_ent->CastToCorpse();
						const uint32 iid = cp->GetItemIDBySlot(static_cast<uint16>(li->slot_id));
						if (iid > 0) {
							const EQ::ItemData* itmd = database.GetItem(iid);
							if (itmd && itmd->IsClassBag() && itmd->BagSlots > 0) {
								// Peek at the corpse's m_item_list for any child at
								// CalcSlotId(equip_slot, i) — if any content exists,
								// promote to auto-loot.
								bool has_content = false;
								LootItem* dummy_bag_data[10] = {};
								if (cp->GetItem(static_cast<uint16>(li->slot_id), dummy_bag_data)) {
									for (int bi = 0; bi < 10; ++bi) {
										if (dummy_bag_data[bi]) { has_content = true; break; }
									}
								}
								if (has_content) {
									LogInfo("[TRILOGY-LOOT] Promoting auto_loot 0→1 for filled bag "
									        "(item_id={} bag_slots={}) — cursor-bag wire path lacks "
									        "post-move content migration",
									        iid, itmd->BagSlots);
									li->auto_loot = 1;
								}
							}
						}
					}
				}

				LogInfo("[TRILOGY-LOOT] ZN_OP_LootItem IN: raw_slot={} → corpse_slot={} lootee={} looter={} raw_auto={} sent_auto={} session=[{}]",
				        raw_slot, li->slot_id, raw_lootee, raw_looter, raw_auto, li->auto_loot,
				        s.trilogy_client->GetName());
				s.trilogy_client->Handle_OP_LootItem(&lootitempkt);
				LogInfo("[TRILOGY-LOOT] ZN_OP_LootItem: Handle_OP_LootItem returned; flushing echo");
				s.trilogy_client->FlushPendingLootEcho();
				LogInfo("[TRILOGY-LOOT] ZN_OP_LootItem: echo flushed");
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
				// Clear the wire→emu slot map + dedup state so the next corpse open
				// starts fresh.  Safe even if the user reopens the same corpse.
				s.trilogy_client->ResetLootSession();
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
		else if (opcode == ZN_OP_EnvDamage && s.trilogy_client) {
			// Client-reported environmental damage (drowning / lava / falling /
			// trap).  v29c reuses OP_Action for this, so the same 28-byte
			// Action_Struct that carries combat notifications outbound arrives
			// inbound meaning "I hurt myself".  EQClassic: Process_Action
			// (client_process.cpp:5918) switches on exactly these type bytes.
			//
			// Guard on the type range: 0x5820 is only ever an environmental
			// report in the client -> server direction, and refusing anything
			// outside 250..253 keeps a malformed or hostile packet from being
			// laundered into an arbitrary self-damage primitive.
			if (plen >= sizeof(Trilogy::structs::Action_Struct)) {
				const auto* ta =
				    reinterpret_cast<const Trilogy::structs::Action_Struct*>(payload);
				const uint8_t dmgtype = static_cast<uint8_t>(ta->type);
				const int32_t damage  = static_cast<int32_t>(ta->damage);

				if (dmgtype < EQ::constants::EnvironmentalDamage::Lava ||
				    dmgtype > EQ::constants::EnvironmentalDamage::Trap) {
					LogInfo("[TrilogyEnvDmg] ignoring 0x5820 with non-environmental "
					        "type={} damage={} from char={}",
					        dmgtype, damage, s.char_id);
				}
				else if (damage < 0) {
					// Upstream turns a negative into 31337 (an instant kill) on a
					// field it reads as unsigned, so the branch is dead there.  We
					// will not resurrect it on a client-supplied value.
					LogInfo("[TrilogyEnvDmg] ignoring 0x5820 with negative damage={} "
					        "type={} from char={}",
					        damage, dmgtype, s.char_id);
				}
				else {
					s.trilogy_client->HandleEnvDamage(dmgtype, damage);
				}
			}
		}
		else if (opcode == ZN_OP_EnvDamage2 && s.trilogy_client) {
			// The 36-byte sibling the client emits in the same tick as 0x5820.
			// EQClassic relays it verbatim so bystanders see the hit
			// (ProcessOP_Default -> entity_list.QueueClients(this, app, false),
			// client_process.cpp:6411) and never reads a damage value out of it.
			// We must not either: it repeats the type and amount from 0x5820, so
			// treating both as damage would charge the player twice.
			//
			// Relayed per-session rather than through entity_list because this is
			// a raw v29c wire format with no modern opcode behind it — only other
			// Trilogy clients can parse it.  Unlike EQClassic we skip the sender:
			// it generated the packet and renders its own hit locally, and the
			// acknowledgement it actually waits for is the HP update that
			// Handle_OP_EnvDamage already sent.
			//
			// The spawn id is overwritten with the sender's own rather than
			// trusted, so a modified client cannot paint damage onto someone else.
			if (plen >= 4) {
				std::vector<uint8_t> relay(payload, payload + plen);
				const uint32_t my_id = s.trilogy_client->GetID();
				memcpy(relay.data(), &my_id, sizeof(my_id));

				const uint64_t my_key = SessionKey(addr, port);
				for (const auto& kv : m_sessions) {
					if (kv.first == my_key || kv.second.state != CONNECTED) {
						continue;
					}
					SendToSession(kv.first, ZN_OP_EnvDamage2,
					              relay.data(), static_cast<uint32_t>(relay.size()));
				}
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

			// Same upload also feeds the breath/air offset hunt.
			LogTrilogyAirProbe(s.char_id, "0x2e20", payload, plen);
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

				// Guild fields, read back out of the client's own profile.  The
				// offsets are settled (see the static_asserts in SendPlayerProfile),
				// so this is now a round-trip check rather than a hunt: whatever we
				// sent should come back unchanged.
				{
					std::string window;
					for (uint32_t off = 4150; off < 4186 && off < plen; ++off) {
						window += fmt::format("{}:{:02X} ", off, payload[off]);
					}
					LogInfo("[TrilogyGuildProbe] char={} ours guildid@4158={} guildrank@4175={} "
					        "raw[4150..4185]=[{}]",
					        s.char_id,
					        static_cast<uint16_t>(cpp->guildid),
					        static_cast<int>(cpp->guildrank),
					        window);
				}

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
			// items.color for legacy leather/chain/plate gear carries the sentinel
			// 0xFF000000 (alpha=FF, RGB=0). v29c stores this per-item and echoes
			// it back in its own OP_WearChange when the player equips the piece
			// (driving the OWN character's local render), which then multiplies
			// the helm texture by RGB=0 = pitch-black. See NormalizeTintColor.
			ci.common.color    = static_cast<uint32>(
				Trilogy::NormalizeTintColor(Strings::ToUnsignedInt(row[39])));
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
		s.trilogy_client   = tc;
		s.eqemu_entity_id  = static_cast<uint16_t>(tc->GetID()); // cache for Tick's stale-pointer guard
		s.counted_in_zone  = true; // legacy fallback if tc ever becomes null post-init

		// Arm the zone-in loop guard at the spawn point so server-side zone-point
		// detection is suppressed until the player walks clear — otherwise a
		// narrow/corridor return trigger (within the detect radius of the spawn)
		// would fire immediately and bounce them straight back (infinite loop).
		tc->ArmTrilogyZoneInGuard(s.pos_x, s.pos_y);
		// (ArmTrilogyZoneInGuard emits its own "guard ARMED" log with the
		//  per-line effective_r and computed threshold under TrilogyZonePointDebug.)

		// Complete the connection: fires EVENT_ENTER_ZONE, UpdateWho, loads zone flags,
		// starts timers.  Outgoing packets from this call flow through TrilogyClient::QueuePacket
		// which translates what it can and silently drops the rest.
		tc->CompleteConnect();

		// Guild appearance on zone-in.
		//
		// v29c keeps TWO copies of a player's guild id and reads a different one
		// depending on what it is doing.  Both were found in eqgame.exe:
		//
		//   actor + 0x90    the entity's guild id.  Written by the spawn parser
		//                   (0x4a3af2, from a word at Spawn_Struct offset 74) and
		//                   by the SpawnAppearance type-22 handler (0x493d31).
		//                   This is what the guild COMMANDS gate on.
		//   profile + 0x103a  the PlayerProfile copy, our pp.guildid.  Written by
		//                   the same appearance handler (0x493cb4).  This is what
		//                   /guildinvite and /guildremove check.
		//
		// The local player's own actor is not built from a Spawn_Struct, so its
		// copy stays at the 0xFFFF "no guild" default for the whole session unless
		// an appearance packet sets it.  Nothing sent one at zone-in: guild_mgr
		// only fires SendGuildSpawnAppearance on a membership change, so a player
		// who was already in a guild when they logged in had actor+0x90 = 0xFFFF.
		//
		// The visible symptom was oddly narrow.  The guild TAG still rendered —
		// that comes from a different actor field the spawn parser does fill — and
		// /guildinvite worked once the profile copy was populated.  Only
		// /guildmotd failed, with "You are not in a guild." printed by the client
		// itself (eqgame.exe 0x4a54c8 checks actor+0x90 against 0xFFFF and 512
		// before it will send anything), so the server never saw a packet at all.
		if (tc->IsInAGuild()) {
			tc->SendGuildSpawnAppearance();
		}

		// Group restoration on zone-in.
		//
		// EQEmu's standard restoration block lives in Client::Handle_Connect_OP_ZoneEntry
		// (client_packet.cpp ~L1527) — read group_id from DB, instantiate the Group in
		// entity_list, LearnMembers + UpdatePlayer, then LoadAndSpawnAllZonedBots.
		// On the Trilogy path OP_ZoneEntry is consumed by TrilogyZoneServer::HandleZoneEntry
		// and Client::Handle_Connect_OP_ZoneEntry never runs, so the group is left
		// unrestored and bots don't re-spawn at the destination zone.  Mirror that block
		// here so the Trilogy zone-in path matches the Titanium semantics.
		uint32 groupid = database.GetGroupID(tc->GetName());
		Group* group = nullptr;
		if (groupid > 0) {
			group = entity_list.GetGroupByID(groupid);
			if (!group) {
				group = new Group(groupid);
				if (group->GetID() != 0) {
					entity_list.AddGroup(group, groupid);
				} else {
					delete group;
					group = nullptr;
				}
			}
			if (!group) {
				Group::RemoveFromGroup(tc);
			}
		}

		if (group) {
			char ln[64]{};
			char MainTankName[64]{};
			char AssistName[64]{};
			char PullerName[64]{};
			char NPCMarkerName[64]{};
			char mentoree_name[64]{};
			int mentor_percent = 0;
			GroupLeadershipAA_Struct GLAA{};
			database.GetGroupLeadershipInfo(group->GetID(), ln, MainTankName, AssistName,
			                                PullerName, NPCMarkerName, mentoree_name,
			                                &mentor_percent, &GLAA);
			group->LearnMembers();

			if (!group->GetLeader()) {
				if (Client* c = entity_list.GetClientByName(ln)) {
					group->SetLeader(c);
				}
			}

			group->SetMainTank(MainTankName);
			group->SetMainAssist(AssistName);
			group->SetPuller(PullerName);
			group->SetNPCMarker(NPCMarkerName);
			group->SetGroupAAs(&GLAA);
			group->SetGroupMentor(mentor_percent, mentoree_name);
			tc->JoinGroupXTargets(group);
			group->UpdatePlayer(tc);
		}

		if (RuleB(Bots, Enabled)) {
			database.botdb.LoadOwnerOptions(tc);
			Bot::LoadAndSpawnAllZonedBots(tc);
		}

		// Group roster sync to the joining v29c client.
		//
		// EQEmu's Group::SendUpdate (groups.cpp) iterates members[] and queues
		// OP_GroupUpdate(groupActJoin) to every OTHER client member — never to
		// the joining player themselves; on Titanium the joining player's
		// group window is populated from PP.groupMembers[] at zone-in.  v29c
		// does NOT read PP.groupMembers — its group window is purely event-
		// driven (see [[project-trilogy-groups]]), so without explicit per-
		// member ADD events the window stays empty on zone-in even though
		// the bots are visible and following.
		//
		// Synthesize one groupActJoin event per existing member, targeted at
		// tc; each one flows through TrilogyClient::QueuePacket →
		// HandleOutgoingGroupUpdate → v29c 0x2640(action=0,othername=member).
		if (group) {
			for (uint32 slot = 0; slot < MAX_GROUP_MEMBERS; ++slot) {
				if (!group->members[slot] || group->members[slot] == tc) {
					continue;
				}
				const char* mem_name = group->members[slot]->GetCleanName();
				if (!mem_name || mem_name[0] == '\0') {
					continue;
				}
				auto outapp = new EQApplicationPacket(OP_GroupUpdate, sizeof(GroupJoin_Struct));
				auto gj = reinterpret_cast<GroupJoin_Struct*>(outapp->pBuffer);
				strncpy(gj->membername, mem_name, sizeof(gj->membername) - 1);
				strncpy(gj->yourname, tc->GetName(), sizeof(gj->yourname) - 1);
				gj->action = groupActJoin;
				tc->QueuePacket(outapp);
				safe_delete(outapp);
			}
		}

		if (group && group->IsLeader(tc)) {
			group->SendLeadershipAAUpdate();
		}

		{
			auto bots_after = entity_list.GetBotsByBotOwnerCharacterID(tc->CharacterID());
			LogInfo("[TrilogyZP] zone-in group restore | char='{}' has_group={} "
			        "group_id={} bots_spawned={}",
			        tc->GetName(), group != nullptr,
			        group ? group->GetID() : 0, (unsigned)bots_after.size());
		}

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
		sp.walkspeed = kTrilogyPlayerWalkSpeed;
		sp.runspeed  = kTrilogyPlayerRunSpeed;
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(tc->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(tc->GetY());
		sp.x_pos     = static_cast<int16_t>(tc->GetX());
		sp.z_pos     = static_cast<int16_t>(tc->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(s.player_spawn_id);
		sp.body_type = static_cast<int16_t>(tc->GetBodyType());
		// Self spawn — only non-zero while this player is charmed.
		sp.pet_owner_id = WireOwnerIdForSession(s, tc->GetOwnerID());
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
		sp.guildrank = Trilogy::structs::TranslateGuildRankToTrilogy(
		    static_cast<uint8_t>(tc->GuildRank()), tc->IsInAGuild());
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
			sp.equipcolors[mi] = static_cast<int32_t>(
				Trilogy::NormalizeTintColor(tc->GetEquipmentColor(static_cast<uint8_t>(mi))));
		// Seed v29c-client-known-material model from the spawn struct equipment.
		if (s.trilogy_client) s.trilogy_client->SeedKnownMaterials(
			static_cast<uint16_t>(sp.spawn_id), sp.equipment);
		// Record for ghost-spawn reconciliation in SendMobHeartbeat.  The
		// reconcile pass skips s.player_spawn_id defensively, so this insert
		// is informational only — kept for symmetry with the other emit sites.
		s.known_spawns.insert(static_cast<uint16_t>(sp.spawn_id));
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
		// Unreliable per Agz's original-Verant documentation in
		// EQClassic/Common/Source/EQPacketManager.cpp:412 — "This is used by
		// the EQ servers for HP and position updates among other things."
		// Loss is harmless because the next HP change (combat/regen) re-sends.
		// See [[project-trilogy-unreliable-a120-wire-format]] for wire layout.
		Trilogy::structs::SpawnHPUpdate_Struct hpu{};
		memset(&hpu, 0, sizeof(hpu));
		hpu.spawn_id = static_cast<int32_t>(s.player_spawn_id);
		hpu.cur_hp   = static_cast<int32_t>(s.trilogy_client->GetHP());
		hpu.max_hp   = static_cast<int32_t>(s.trilogy_client->GetMaxHP());
		SendApp(addr, port, s, 0xb220,
		        reinterpret_cast<const uint8_t*>(&hpu), sizeof(hpu),
		        /*ack_req=*/false);

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
		// Stamina (5721) unreliable: periodic refresh.  Tick re-sends every 5 s
		// so a UDP drop is invisible — same supersede-by-next-update property
		// as A120/B220 per Agz EQPacketManager.cpp:412.  fatigue stays 0 so
		// client-side endurance never depletes.  food/water mirror the real
		// PlayerProfile values so the v29c client can drive its own auto-consume
		// loop (it sends ZN_OP_ConsumeFoodDrink 0x5621 when hunger/thirst drops
		// below its threshold).  The engine's Client::DoStaminaHungerUpdate
		// (client_process.cpp:1965) decrements m_pp.hunger_level/thirst_level
		// every 46 s; the OP_Stamina it queues is silently dropped by
		// TrilogyClient (no translator), so this refresh is the only path to
		// the wire for v29c hunger/thirst.
		// See [[project-trilogy-unreliable-a120-wire-format]] for wire layout.
		const auto& pp = s.trilogy_client->GetPP();
		Trilogy::structs::Stamina_Struct sta{};
		memset(&sta, 0, sizeof(sta));
		sta.food    = static_cast<int16_t>(std::min(std::max(pp.hunger_level, 0), 6000));
		sta.water   = static_cast<int16_t>(std::min(std::max(pp.thirst_level, 0), 6000));
		sta.fatigue = 0;
		SendApp(addr, port, s, ZN_OP_Stamina,
		        reinterpret_cast<const uint8_t*>(&sta), sizeof(sta),
		        /*ack_req=*/false);
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
			// Illusion target must match the spawn-struct name field — both
			// flow through TrilogyWireName so duplicate-named NPCs get
			// distinct wire identities and v29c's by-name lookup resolves
			// to the right entity per instance.
			FillIllusionBuf(il_buf, TrilogyWireName(npc),
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
				wc.color        = static_cast<int32_t>(
					Trilogy::NormalizeTintColor(npc->GetEquipmentColor(EQ::textures::armorHead)));
				SendApp(addr, port, s, 0x9220,
				        reinterpret_cast<const uint8_t*>(&wc),
				        static_cast<uint32_t>(sizeof(wc)));
				// Update v29c-client-known-material model: helm slot now reflects helmtex.
				if (s.trilogy_client) s.trilogy_client->RecordKnownMaterial(
					static_cast<uint16_t>(npc->GetID()), 0, helmtex);
			}
		}

		// Bots: same Illusion fixup for face data.  Bots are NPC=0 (player
		// nameplate) and render the helm through equipment[0] just like
		// Playerbots, so no follow-up WearChange is needed.
		for (Bot* bot : entity_list.GetBotList()) {
			if (!bot || !IsPlayerRace(bot->GetRace())) continue;
			uint8_t il_buf[72];
			FillIllusionBuf(il_buf, TrilogyWireName(bot),
			    static_cast<int16_t>(bot->GetRace()),
			    static_cast<int16_t>(bot->GetGender()),
			    static_cast<int16_t>(-1),   // 0xFFFF: keep current texture/mode
			    static_cast<int16_t>(-1),   // 0xFFFF: keep current helm
			    static_cast<int16_t>(bot->GetLuclinFace()));
			SendApp(addr, port, s, 0x9120, il_buf, 72);
		}

		// Other players.  They were the one player-race class missing from this
		// loop, which is what made a real player's face default for anyone who
		// zoned in after them: the face byte IS in the bulk spawn struct, but
		// v29c does not apply it to entities delivered in the bulk 0x6121 --
		// that is the whole reason this fixup pass exists for NPCs and bots.
		//
		// Only the bulk direction was broken, which is why the bug looked
		// order-dependent: a player who spawns MID-session arrives as a single
		// 0x6121 through SendPlayerSpawnPermanent, and a single spawn's face
		// byte is honoured.  So whoever logged in first saw the second player
		// correctly, and only the later arrival saw a default face -- with the
		// asymmetry hidden entirely if the second character's face happens to
		// be the default one anyway.
		for (const auto& kv : entity_list.GetClientList()) {
			Client* other = kv.second;
			if (!other || !other->InZone()) continue;
			if (s.trilogy_client && other == s.trilogy_client) continue; // not self
			if (!IsPlayerRace(other->GetRace())) continue;
			uint8_t il_buf[72];
			FillIllusionBuf(il_buf, other->GetCleanName(),
			    static_cast<int16_t>(other->GetRace()),
			    static_cast<int16_t>(other->GetGender()),
			    static_cast<int16_t>(-1),   // 0xFFFF: keep current texture/mode
			    static_cast<int16_t>(-1),   // 0xFFFF: keep current helm
			    static_cast<int16_t>(other->GetLuclinFace()));
			SendApp(addr, port, s, 0x9120, il_buf, 72);
			LogInfo("[TrilogyFace] zone-in illusion | observer=[{}] target=[{}] "
			        "race={} gender={} face={}",
			        s.char_name, other->GetCleanName(),
			        other->GetRace(), other->GetGender(),
			        static_cast<int>(other->GetLuclinFace()));
		}
	}

	// Tell the arriving client who in the zone is already LFG.  The toggle
	// broadcast only reaches people who were present when it happened, so
	// without this pass a player who zones in never learns about anyone's
	// existing flag.  One 0xf021 per LFG client, entity-id form.
	{
		int lfg_sent = 0;
		for (const auto& kv : entity_list.GetClientList()) {
			Client* other = kv.second;
			if (!other || !other->InZone() || !other->IsLFG()) continue;
			if (s.trilogy_client && other == s.trilogy_client) continue; // self is local
			uint8_t buf[8] = {};
			const int16_t eid   = static_cast<int16_t>(other->GetID());
			const int32_t value = 1;
			memcpy(buf + 0, &eid,   sizeof(eid));
			memcpy(buf + 4, &value, sizeof(value));
			SendApp(addr, port, s, ZN_OP_LFG, buf, sizeof(buf));
			++lfg_sent;
		}
		if (lfg_sent > 0) {
			LogInfo("[TrilogyLFG] zone-in | told [{}] about {} LFG player(s)",
			        s.char_name, lfg_sent);
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

	// ============================================================
	// Wide-boundary terrain snap (cross-zone FindBestZ workaround) — LATE PATH
	// ============================================================
	// The departing zone's CheckTraditionalZonePoints (zoning.cpp ~L435/L568)
	// applies a static +25 skydrop to the DB-stored target_z because that
	// process has not loaded the destination zone's collision map.  When the
	// wide-boundary slide lands the player far from the DB's reference point
	// on steeply-sloped terrain (e.g. ecommons->commons at large delta_y), the
	// real terrain can be hundreds of units below the DB Z and the EQ client's
	// local physics drops the player into/under-world.
	//
	// PRIMARY snap now runs at CONNECTING2, before SendPlayerProfile — see
	// the "wide-boundary terrain-snap (pre-PP)" block earlier in the file.
	// That path prevents the initial PP from ever telling the client a Z
	// hundreds of units above terrain and avoids the fall-through visual.
	//
	// This LATE snap remains as a defensive fallback for corner cases where
	// s.pos_z drifts between CONNECTING2 and here (e.g. a future path that
	// mutates it during the CONNECTING3/4 handshake).  It is idempotent with
	// the pre-PP snap: after the pre-PP snap runs, drop here evaluates to
	// ~3u and the "within tolerance, no snap" branch fires without changing
	// anything.  Only fires for wide-boundary zone-ins (s.pending_heading_sync);
	// narrow doors use an explicit DB target_z and don't need the snap.
	if (s.trilogy_client && s.pending_heading_sync && zone && zone->zonemap) {
		// See the pre-PP snap in SendPlayerProfile for the full rationale.
		// Two-pass probe: standard (pos_z + 5), fall back to elevated
		// (pos_z + 200) if pass 1 hit sub-terrain or missed.  Snap when
		// drop > 40 (above walkable) OR drop < -10 (below walkable — client
		// physics can't push up).
		float terrain_z = BEST_Z_INVALID;
		float drop      = 0.0f;
		const char* pass_tag = "";

		{
			glm::vec3 start(s.pos_x, s.pos_y, s.pos_z + 5.0f);
			float t = zone->zonemap->FindBestZ(start, nullptr);
			if (t != BEST_Z_INVALID) {
				float d = s.pos_z - t;
				if (std::fabs(d) < 150.0f) {
					terrain_z = t;
					drop      = d;
					pass_tag  = "low";
				}
			}
		}

		if (terrain_z == BEST_Z_INVALID) {
			const float probe_start = s.pos_z + 200.0f;
			glm::vec3 start(s.pos_x, s.pos_y, probe_start);
			float t = zone->zonemap->FindBestZ(start, nullptr);
			if (t != BEST_Z_INVALID && std::fabs(t - probe_start) > 3.0f) {
				float d = s.pos_z - t;
				if (std::fabs(d) < 150.0f) {
					terrain_z = t;
					drop      = d;
					pass_tag  = "high";
				}
			}
		}

		// Asymmetric threshold (LATE snap):
		//   snap_down (drop > 10u): player 10+u ABOVE walkable. Client physics
		//       recovers via gravity — brief drop animation, no gameplay harm.
		//       Only snap on the "obvious skydrop" case to avoid re-triggering
		//       on 1-3u terrain-probe jitter.
		//   snap_up (drop < -2u): player 2+u BELOW walkable = embedded in the
		//       terrain mesh. Client gravity only pulls down, so there is NO
		//       recovery mechanism — the player falls through the world. Tight
		//       threshold (2u buffer for probe jitter) so plane-crossing
		//       arrivals where the DB target_z is authored for one point but
		//       the slid arrival XY has slightly higher ground (a few units of
		//       slope) always land above ground.
		//   Empirical from 2026-07-04 ecommons(4940,-734) arrival: pos_z=-50,
		//   terrain_z=-44.96, drop=-5.04. Previous symmetric ±10u tolerance
		//   left player 5u embedded → fall-through-world.
		const bool snap_down = (drop >  10.0f);
		const bool snap_up   = (drop <  -2.0f);
		if (terrain_z != BEST_Z_INVALID && (snap_down || snap_up)) {
			float new_z = terrain_z + 3.0f;
			LogInfo("[TrilogyZP] wide-boundary terrain-snap ({}) | char [{}] zone [{}] "
			        "pos ({:.1f},{:.1f}) orig_z={:.2f} terrain_z={:.2f} drop={:+.2f} -> new_z={:.2f}",
			        pass_tag, s.char_name, s.zone_short, s.pos_x, s.pos_y,
			        s.pos_z, terrain_z, drop, new_z);
			s.pos_z = new_z;
			s.trilogy_client->SetPosition(s.pos_x, s.pos_y, new_z);
		} else if (terrain_z != BEST_Z_INVALID) {
			LogInfo("[TrilogyZP] wide-boundary terrain-check ({}) | char [{}] zone [{}] "
			        "pos ({:.1f},{:.1f}) pos_z={:.2f} terrain_z={:.2f} drop={:+.2f} (within tolerance, no snap)",
			        pass_tag, s.char_name, s.zone_short, s.pos_x, s.pos_y,
			        s.pos_z, terrain_z, drop);
		} else {
			// Same cross-zone DB lookup as pre-PP: find the DB target that
			// sent us here and use it as the funnel point.  If pre-PP
			// already ran and applied the funnel, s.pos_x/y/z will now sit
			// on walkable geometry and this branch won't re-fire (the
			// initial probe will succeed).  Kept as defensive fallback for
			// paths where the pre-PP fallback didn't run.
			float funnel_x = 0.0f, funnel_y = 0.0f, funnel_z = 0.0f;
			bool  funnel_found = false;
			{
				auto q = fmt::format(
					"SELECT target_x, target_y, target_z FROM zone_points "
					"WHERE target_zone_id = {} "
					"  AND target_x != 999999 AND target_x != -999999 "
					"  AND target_y != 999999 AND target_y != -999999 "
					"  AND ABS(target_y - {}) < 100 "
					"ORDER BY ABS(target_y - {}) ASC LIMIT 1",
					zone->GetZoneID(), s.pos_y, s.pos_y);
				auto r = database.QueryDatabase(q);
				if (r.RowCount() > 0) {
					auto row = r.begin();
					funnel_x = Strings::ToFloat(row[0]);
					funnel_y = Strings::ToFloat(row[1]);
					funnel_z = Strings::ToFloat(row[2]);
					funnel_found = true;
				}
			}
			if (funnel_found) {
				LogInfo("[TrilogyZP] wide-boundary OFF-MAP DB-funnel (late) | char [{}] zone [{}] "
				        "seamless XY ({:.1f},{:.1f}) off-map -> DB target ({:.1f},{:.1f},{:.2f})",
				        s.char_name, s.zone_short,
				        s.pos_x, s.pos_y,
				        funnel_x, funnel_y, funnel_z);
				s.pos_x = funnel_x;
				s.pos_y = funnel_y;
				s.pos_z = funnel_z + 3.0f;
				s.trilogy_client->SetPosition(funnel_x, funnel_y, funnel_z + 3.0f);
			} else {
				const glm::vec4 safe = zone->GetSafePoint();
				const bool safe_valid = (safe.x != 0.0f || safe.y != 0.0f || safe.z != 0.0f);
				if (safe_valid) {
					LogInfo("[TrilogyZP] wide-boundary OFF-MAP safe-point (late) | char [{}] zone [{}] "
					        "seamless XY ({:.1f},{:.1f}) off-map AND no DB target found -> safe ({:.1f},{:.1f},{:.2f})",
					        s.char_name, s.zone_short,
					        s.pos_x, s.pos_y,
					        safe.x, safe.y, safe.z);
					s.pos_x = safe.x;
					s.pos_y = safe.y;
					s.pos_z = safe.z + 3.0f;
					s.trilogy_client->SetPosition(safe.x, safe.y, safe.z + 3.0f);
				} else {
					LogInfo("[TrilogyZP] wide-boundary OFF-MAP (late) | char [{}] zone [{}] "
					        "seamless XY ({:.1f},{:.1f}) off-map, no DB target, no safe point — leaving pos alone",
					        s.char_name, s.zone_short, s.pos_x, s.pos_y);
				}
			}
		}
	}

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

	// Populate persistent buffs from character_buffs so buffs survive zoning.  v29c has
	// no OP_Buff packet at zone-in — the client reads its buff bar exclusively from the
	// PlayerProfile SpellBuff_Struct array (see EQClassic\Zone\Source\client.cpp:372
	// where they populate this same field before SetPlayerProfile).  SaveBuffs on the
	// source zone (Client::Save → SaveBuffs) writes character_buffs before we get here,
	// so this read picks up the freshly persisted state.  The server-side reapply of
	// spell effects (illusions, procs, appearance) is handled by CompleteConnect's
	// buff loop after Client::buffs[] is filled via LoadBuffs in InitTrilogyFields.
	{
		auto br = database.QueryDatabase(fmt::format(
		    "SELECT `slot_id`, `spell_id`, `caster_level`, `ticsremaining`, `instrument_mod` "
		    "FROM `character_buffs` WHERE `character_id` = {}",
		    s.char_id));
		if (br.Success()) {
			for (auto row = br.begin(); row != br.end(); ++row) {
				const int slot     = row[0] ? Strings::ToInt(row[0]) : -1;
				const int spell_id = row[1] ? Strings::ToInt(row[1]) : 0;
				const int level    = row[2] ? Strings::ToInt(row[2]) : 0;
				const int tics     = row[3] ? Strings::ToInt(row[3]) : 0;
				const int inst_mod = row[4] ? Strings::ToInt(row[4]) : 10;
				if (slot < 0 || slot >= 15) continue;               // v29c BUFF_COUNT = 15
				if (spell_id <= 0 || spell_id >= 0xFFFF) continue;  // wire is int16
				pp.buffs[slot].spellid       = static_cast<int16_t>(spell_id);
				pp.buffs[slot].duration      = static_cast<int32_t>(tics);
				pp.buffs[slot].level         = static_cast<int8_t>(level);
				pp.buffs[slot].visable       = 2; // 2 = visible + timed (see EQClassic client.cpp:379)
				pp.buffs[slot].bard_modifier = static_cast<int8_t>(inst_mod > 0 ? inst_mod : 10);
			}
		}
	}

	// ---- character_data ----
	{
		auto q = fmt::format(
			"SELECT `name`, `last_name`, `gender`, `deity`, `race`, `class`,"
			" `level`, `exp`, `mana`, `face`, `cur_hp`,"
			" `str`, `sta`, `cha`, `dex`, `int`, `agi`, `wis`,"
			" `y`, `x`, `z`, `heading`, `zone_id`,"
			" `hunger_level`, `thirst_level`, `anon`, `points`, `gm`,"
			" `birthday`, `time_played` "
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
		// v29c PP.exp is the RAW cumulative experience value (the client computes the
		// XP-bar fill internally from level + exp using its own GetEXPForLevel formula).
		// Matches EQClassic\Zone\Source\client.cpp:590 (`pp.exp = target_exp`) and
		// `EQClassic\Common\Include\PlayerProfile.h:61 (\"Current Experience\")`.
		// The earlier 0-330-normalized write caused \"bar full at login\" at any level
		// > 1: a small normalized value (e.g. 165) became `(165 - base_for_level)`,
		// which underflows as unsigned → huge value → bar clips to 100%.
		pp.exp             = static_cast<int32_t>(Strings::ToInt(row[7]));
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

		// Breath meter.  Drowning on v29c is client-authoritative: the client
		// counts air down while submerged and reports the damage back as
		// OP_Action (0x5820) type 0xFB.  It re-fills on its own once your head
		// is above water, but at zone-in it seeds from the PlayerProfile — and
		// we zero-fill the struct, so anyone who arrives UNDERWATER (swimming a
		// zone line, e.g. qeynos -> qcat) started at zero air and drowned on the
		// spot: 185 damage per tick, ten ticks, then a client-reported OP_Death.
		//
		// Stamping it to full here is the same fix every other EQEmu client path
		// already has — client_packet.cpp:1647 "Reset to max so they dont drown
		// on zone in if its underwater", and EQMacEmu's equivalent for the Mac
		// client (client_packet.cpp:1316).  A zone-in is always a fresh arrival,
		// so full is the correct value; there is nothing to carry across.
		//
		// 45 is what the client itself reports at rest on dry land, captured
		// from its own PP upload (see [TrilogyAirProbe]).  Writing more than the
		// client's own maximum has not been tested, so we do not inflate it.
		pp.air_supply      = kTrilogyFullAirSupply;

		// /played fields.  The v29c client's /played handler is entirely
		// client-side: it reads two adjacent int32s from the PP and formats
		// them locally.  Offsets pinned by sigil probe 2026-08-15:
		//   birthday_time   (byte 4160) — unix timestamp of char creation
		//   time_played_min (byte 4164) — cumulative minutes played
		// Both columns are populated by the modern EQEmu character path:
		// character_data.birthday is set at CharCreate (trilogy_world.cpp)
		// and character_data.time_played is accumulated by Client::Save()
		// via TotalSecondsPlayed.  The seed in InitTrilogyFields (client.cpp)
		// prevents the very first Trilogy-session Save() from clobbering
		// the stored total.
		pp.birthday_time   = static_cast<int32_t>(Strings::ToUnsignedInt(row[28]));
		pp.time_played_min = static_cast<int32_t>(Strings::ToUnsignedInt(row[29]));

		// Guild identity.  These two were never written, and that is not the
		// cosmetic gap it looks like: the client gates /guildinvite,
		// /guildremove and /guildmotd on its OWN copy of the rank, and a
		// zero-filled guildrank is GUILDRANK_MEMBER.  A real guild leader was
		// therefore told "You are not a guild officer or leader, and therefore
		// cannot invite members to join your guild" — the client refused before
		// a packet was ever sent, which is why the invite opcode never appeared
		// in the log.
		//
		// The guild TAG comes from spawn data, which is why this stayed hidden:
		// the tag rendered correctly while the commands did not.
		//
		// The offsets are confirmed against eqgame.exe, not taken on trust from
		// EQClassic's header (which is four bytes off for deity in this same
		// region): the client's own /guildinvite gate reads them at object
		// 0x103a / 0x104b.  See the static_asserts below.
		{
			uint32 g_id   = GUILD_NONE;
			uint8  g_rank = GUILD_RANK_NONE;
			auto   gq     = fmt::format(
			    "SELECT `guild_id`, `rank` FROM `guild_members` WHERE `char_id` = {} LIMIT 1",
			    s.char_id);
			auto gr = database.QueryDatabase(gq);
			if (gr.RowCount() > 0) {
				auto grow = gr.begin();
				if (grow[0] && Strings::ToInt(grow[0]) > 0) {
					g_id   = static_cast<uint32>(Strings::ToInt(grow[0]));
					g_rank = grow[1] ? static_cast<uint8>(Strings::ToInt(grow[1]))
					                 : static_cast<uint8>(GUILD_RANK_NONE);
				}
			}
			const bool in_guild = (g_id != GUILD_NONE);
			pp.guildid   = in_guild ? static_cast<int16_t>(g_id)
			                        : static_cast<int16_t>(0xFFFF);
			pp.guildrank = Trilogy::structs::TranslateGuildRankToTrilogy(g_rank, in_guild);
			LogInfo("[TrilogyGuild] PP char={} guild_id={} emu_rank={} wire_rank={}",
			        s.char_id, g_id, g_rank, static_cast<int>(pp.guildrank));
		}

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

		// ============================================================
		// Wide-boundary terrain snap (PRE-PP) — see also HandleZoneInComplete
		// ============================================================
		// Wide-boundary sliding preserves the player's exit coordinate on the
		// wildcard axis, but the departing zone can't run FindBestZ against the
		// destination's collision map — so it applies a static +25 skydrop to
		// the DB target_z.  When the slid arrival XY has walkable ground far
		// from the DB reference (elevated slopes, steep dunes, valleys), the
		// skydrop lands the player above OR below the true walkable surface.
		//
		// FindBestZ's cast-down bias makes finding the true walkable Z
		// finicky:
		//   - Start probe LOW (pos_z + 5): if walkable is BELOW start, we
		//     find it; if walkable is ABOVE start, we blow through walkable
		//     and hit sub-terrain at the world floor (observed: commons
		//     probe from -21 returning -290 when actual walkable is +8..+30
		//     on a desert slope).
		//   - Start probe VERY HIGH (Z=500): raycast library misbehaves
		//     above the navmesh AABB, returns start_z verbatim (observed).
		//
		// Two-pass strategy:
		//   Pass 1: probe from pos_z + 5 (standard).  If drop is "plausibly
		//           walkable" (|drop| < 150u), use it.
		//   Pass 2: only if pass 1 failed or hit sub-terrain — probe from
		//           pos_z + 200.  This is high enough to be above typical
		//           outdoor walkable slopes at wide-boundary XYs, low enough
		//           to stay inside most zones' navmesh AABB.  Sanity-check
		//           the return against the probe start to catch bogus
		//           "returned = start" values from the raycast library.
		//
		// Snap trigger: drop > 40u (player above walkable, standard skydrop
		// overshoot) OR drop < -10u (player BELOW walkable — can't recover
		// via client physics since gravity only pulls down).  |drop| capped
		// at 150u in each pass to reject residual sub-terrain hits.
		//
		// Narrow doors use an explicit DB target_z authored near the exact
		// arrival spot and don't need this snap — gate on boundary_is_wide.
		if (boundary_is_wide && zone && zone->zonemap) {
			float terrain_z = BEST_Z_INVALID;
			float drop      = 0.0f;
			const char* pass_tag = "";

			// Pass 1: standard probe height.
			{
				glm::vec3 start(s.pos_x, s.pos_y, s.pos_z + 5.0f);
				float t = zone->zonemap->FindBestZ(start, nullptr);
				if (t != BEST_Z_INVALID) {
					float d = s.pos_z - t;
					if (std::fabs(d) < 150.0f) {
						terrain_z = t;
						drop      = d;
						pass_tag  = "low";
					}
				}
			}

			// Pass 2: elevated probe if pass 1 didn't find plausible walkable.
			if (terrain_z == BEST_Z_INVALID) {
				const float probe_start = s.pos_z + 200.0f;
				glm::vec3 start(s.pos_x, s.pos_y, probe_start);
				float t = zone->zonemap->FindBestZ(start, nullptr);
				// Reject "returned start Z verbatim" (raycast above-AABB bogus).
				// Reject sub-terrain hits (|drop| >= 150u) — same as pass 1.
				if (t != BEST_Z_INVALID && std::fabs(t - probe_start) > 3.0f) {
					float d = s.pos_z - t;
					if (std::fabs(d) < 150.0f) {
						terrain_z = t;
						drop      = d;
						pass_tag  = "high";
					}
				}
			}

			// Asymmetric threshold (pre-PP):
			//   snap_down (drop > 10u): player 10+u ABOVE walkable — client
			//       physics recovers via gravity, brief drop animation. Only
			//       snap on obvious skydrop to avoid re-triggering on probe
			//       jitter.
			//   snap_up (drop < -2u): player 2+u BELOW walkable = embedded in
			//       terrain. Client gravity only pulls down → no recovery →
			//       fall-through-world. Tight threshold (2u buffer for probe
			//       jitter) so plane-crossing arrivals where the DB target_z
			//       is authored for one point but the slid arrival XY has
			//       slightly different ground (any slope) still land above.
			//   Empirical from 2026-07-04: ecommons(4940,-734) plane-crossing
			//   arrival at pos_z=-50 vs terrain_z=-44.96 (drop=-5.04) fell
			//   through under the previous symmetric ±10u tolerance.
			const bool snap_down = (drop >  10.0f);
			const bool snap_up   = (drop <  -2.0f);
			if (terrain_z != BEST_Z_INVALID && (snap_down || snap_up)) {
				float new_z = terrain_z + 3.0f;
				LogInfo("[TrilogyZP] wide-boundary terrain-snap (pre-PP, {}) | char [{}] zone [{}] "
				        "pos ({:.1f},{:.1f}) orig_z={:.2f} terrain_z={:.2f} drop={:+.2f} -> new_z={:.2f}",
				        pass_tag, s.char_name, s.zone_short, s.pos_x, s.pos_y,
				        s.pos_z, terrain_z, drop, new_z);
				pp.z    = new_z;
				s.pos_z = new_z;
			} else if (terrain_z == BEST_Z_INVALID) {
				// Both probes rejected — the seamless slide landed at an XY
				// where the destination has no walkable geometry.  This is
				// the freporte<->NRO case: freeport walkable X range is
				// wider than NRO's server-side walkable X range at the
				// boundary Y, so a player crossing at freporte's west/east
				// wall arrives at an NRO X that our server map doesn't
				// cover.  User confirmed 2026-07-02 that Titanium in this
				// case funnels the player to the DB-authored arrival point
				// ("middle of the zoneline") rather than trying to preserve
				// the seamless X.
				//
				// Cross-zone DB lookup: find any zone_point in ANY zone that
				// targets THIS zone with a target_y close to our arrival Y
				// (within 100u to allow for the +15 push).  That row is the
				// source zone's zone_point that just fired to send us here,
				// and its (target_x, target_y, target_z) is the DB-authored
				// funnel spot.  If found, use it verbatim.  Otherwise fall
				// back to the zone safe point so we still don't fall through.
				float funnel_x = 0.0f, funnel_y = 0.0f, funnel_z = 0.0f;
				bool  funnel_found = false;
				{
					auto q = fmt::format(
						"SELECT target_x, target_y, target_z FROM zone_points "
						"WHERE target_zone_id = {} "
						"  AND target_x != 999999 AND target_x != -999999 "
						"  AND target_y != 999999 AND target_y != -999999 "
						"  AND ABS(target_y - {}) < 100 "
						"ORDER BY ABS(target_y - {}) ASC LIMIT 1",
						zone->GetZoneID(), s.pos_y, s.pos_y);
					auto r = database.QueryDatabase(q);
					if (r.RowCount() > 0) {
						auto row = r.begin();
						funnel_x = Strings::ToFloat(row[0]);
						funnel_y = Strings::ToFloat(row[1]);
						funnel_z = Strings::ToFloat(row[2]);
						funnel_found = true;
					}
				}
				if (funnel_found) {
					LogInfo("[TrilogyZP] wide-boundary OFF-MAP DB-funnel (pre-PP) | char [{}] zone [{}] "
					        "seamless XY ({:.1f},{:.1f}) off-map -> DB target ({:.1f},{:.1f},{:.2f})",
					        s.char_name, s.zone_short,
					        s.pos_x, s.pos_y,
					        funnel_x, funnel_y, funnel_z);
					pp.x    = funnel_x;
					pp.y    = funnel_y;
					pp.z    = funnel_z + 3.0f;
					s.pos_x = funnel_x;
					s.pos_y = funnel_y;
					s.pos_z = funnel_z + 3.0f;
				} else {
					const glm::vec4 safe = zone->GetSafePoint();
					const bool safe_valid = (safe.x != 0.0f || safe.y != 0.0f || safe.z != 0.0f);
					if (safe_valid) {
						LogInfo("[TrilogyZP] wide-boundary OFF-MAP safe-point (pre-PP) | char [{}] zone [{}] "
						        "seamless XY ({:.1f},{:.1f}) off-map AND no DB target found -> safe ({:.1f},{:.1f},{:.2f})",
						        s.char_name, s.zone_short,
						        s.pos_x, s.pos_y,
						        safe.x, safe.y, safe.z);
						pp.x    = safe.x;
						pp.y    = safe.y;
						pp.z    = safe.z + 3.0f;
						s.pos_x = safe.x;
						s.pos_y = safe.y;
						s.pos_z = safe.z + 3.0f;
					} else {
						LogInfo("[TrilogyZP] wide-boundary OFF-MAP (pre-PP) | char [{}] zone [{}] "
						        "seamless XY ({:.1f},{:.1f}) off-map, no DB target found, no safe point — "
						        "leaving pos alone (fall-through likely)",
						        s.char_name, s.zone_short, s.pos_x, s.pos_y);
					}
				}
			}
		}
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
	// Both pinned from eqgame.exe rather than from EQClassic's header: the
	// client's /guildinvite gate reads the profile at object offset 0x103a and
	// 0x104b (0x4c9f07 and 0x4c9f22), and the SpawnAppearance type-22 handler
	// writes the same word at 0x103a (0x493cb4).  Object offsets run four bytes
	// behind struct offsets because the object holds the profile with the
	// checksum stripped — the same relationship deity_wire above is pinned by.
	static_assert(offsetof(Trilogy::structs::PlayerProfile_Struct, guildid) == 4158,
	              "guildid must be at PP struct byte 4158 (= client object 0x103a)");
	static_assert(offsetof(Trilogy::structs::PlayerProfile_Struct, guildrank) == 4175,
	              "guildrank must be at PP struct byte 4175 (= client object 0x104b)");
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
	// Position: take from the session, not the DB row.  SendPlayerProfile (called
	// immediately before us) may have applied a wide-boundary terrain snap to
	// s.pos_z that the DB row does not yet reflect (the character_data save
	// happens later via Client::Save).  Reading from the DB here would ship a
	// stale Z to the client's self-spawn, and OP_ZoneEntry is the packet that
	// sets the rendered position — that's the "fell through geometry" symptom
	// that persisted even after the PP-time snap fix.  s.pos_heading is already
	// the decoded (positive) value; no sign flip is needed.  DB row columns 7-10
	// remain SELECTed to keep the query stable for other fields.
	sze.y         = s.pos_y;
	sze.x         = s.pos_x;
	sze.z         = s.pos_z;
	sze.heading   = s.pos_heading;
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
			// items.color for legacy leather/chain gear is 0xFF000000 — modern
			// clients ignore that via PP.item_tint.UseTint, but v29c would apply
			// it as an opaque black tint (pitch-black helm). See NormalizeTintColor.
			sze.helmcolor = Trilogy::NormalizeTintColor(
				static_cast<uint32_t>(Strings::ToUnsignedInt(hrow[1])));
		}
	}

	// Walk/run speeds — see kTrilogyPlayer*Speed constants near top of file.
	sze.walkspeed = kTrilogyPlayerWalkSpeed;
	sze.runspeed  = kTrilogyPlayerRunSpeed;

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

	// Per-zone perma-twilight lock: only the wire hour/minute is overridden,
	// so zone->zone_time keeps advancing and gameplay logic (spell durations,
	// guard schedules, /time output) reads real time. If more zones ever need
	// this, promote the short_name match to a table/rule list.
	//
	// Wire convention: v29c interprets byte N as (N-1):00 in 24-hour clock —
	// byte 1 = midnight, byte 13 = noon, byte 18 = 5 PM (red sunset), byte 24
	// = 11 PM. `Zone::SetTime` (see zone.cpp) applies the same +1 shift, which
	// is why `#time 17` produces the red 5 PM sky. The rule value is expressed
	// in the human 0-23 range so users dial in the same number they'd pass to
	// `#time` and get the identical sky.
	if (zone && IsTrilogyTimeLocked(zone->GetShortName())) {
		const int lock_h = std::clamp(RuleI(Zone, TrilogyAirplaneLockHour), 0, 23);
		const int lock_m = std::clamp(RuleI(Zone, TrilogyAirplaneLockMinute), 0, 59);
		tod.hour   = static_cast<int8_t>(lock_h + 1);
		tod.minute = static_cast<int8_t>(lock_m);
	}

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

	// Diagnostic: decode the v29c CLIENT's outgoing delta_x/y/z bitfield —
	// this is our calibration anchor for kVelocityWireScale.  The v29c
	// client computes these deltas from its own local player motion, so
	// the values it sends while the player is running represent "what the
	// client itself considers a sensible delta for that velocity."  Match
	// our outbound encoding to this scale and NPC trajectory extrapolation
	// will look right to the client.  Rate-limited per session.
	{
		uint64_t now_dbg = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		if (upd.anim_type != 0 && now_dbg - s.last_delta_dbg_in_ms >= kDeltaDebugMs) {
			s.last_delta_dbg_in_ms = now_dbg;
			int32_t dx, dy, dz;
			ReadDeltaBitfield(&upd, dx, dy, dz);
			LogInfo("[Trilogy/delta IN] char=[{}] anim={} heading={} dx={} dy={} dz={} "
			        "|v|={:.2f}",
			        s.char_name,
			        static_cast<int>(upd.anim_type),
			        static_cast<int>(static_cast<uint8_t>(upd.heading)),
			        dx, dy, dz,
			        std::sqrt(static_cast<float>(dx * dx + dy * dy)));
		}
	}

	// ── Rotation probe ───────────────────────────────────────────────────
	// The [Trilogy/delta IN] diagnostic above is gated on anim_type != 0, so a
	// player pivoting on the spot -- no movement, anim_type 0 -- never reaches
	// it and we have no measurement of what the client actually sends while
	// turning.  That gap is why the first attempt at smoothing observed
	// rotation was aimed at our own broadcast cadence: a guess, and wrong.
	//
	// Two numbers decide where the real ceiling is, and this answers both:
	//   updates/sec while the heading byte is changing — if the CLIENT only
	//     reports a few times a second, no outbound cadence can beat that and
	//     interpolation is the only route;
	//   the client's reported delta_heading — EQClassic relays this field for
	//     PCs (client_process.cpp:2390 -> mob.cpp:571), and it is the only
	//     mechanism that lets v29c sweep between two headings instead of
	//     snapping.  If it arrives non-zero, the relay is worth doing; if it
	//     is always zero, the field is not the answer here either.
	{
		const uint8_t hb_now  = static_cast<uint8_t>(upd.heading);
		const uint64_t now_rot = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());

		if (s.rot_window_start_ms == 0) s.rot_window_start_ms = now_rot;
		++s.rot_updates;
		if (hb_now != s.rot_last_heading_wire) {
			++s.rot_heading_changes;
			s.rot_last_heading_wire = hb_now;
		}
		if (upd.delta_heading != 0) ++s.rot_nonzero_delta_heading;

		const uint64_t span = now_rot - s.rot_window_start_ms;
		if (span >= 1000) {
			if (s.rot_heading_changes > 0) {
				LogInfo("[Trilogy/rot IN] char=[{}] updates={} heading_changes={} "
				        "nonzero_delta_heading={} last_delta_heading={} span_ms={}",
				        s.char_name, s.rot_updates, s.rot_heading_changes,
				        s.rot_nonzero_delta_heading,
				        static_cast<int>(upd.delta_heading), span);
			}
			s.rot_window_start_ms       = now_rot;
			s.rot_updates               = 0;
			s.rot_heading_changes       = 0;
			s.rot_nonzero_delta_heading = 0;
		}
	}

	s.pos_x = x; s.pos_y = y; s.pos_z = z; s.pos_heading = heading;

	// Update entity_list position so NPC aggro, proximity, and Titanium broadcasts work.
	if (s.trilogy_client) {
		// First ZN_OP_ClientUpdate after zone-in (or zone-out) signals the client's 3D world
		// is live.  Release any buffered spawn/ground packets and resume mob heartbeats.
		const bool was_zoning = s.trilogy_client->IsZoning();
		if (was_zoning)
			s.trilogy_client->OnClientReady();

		// Stash the client's OWN signed anim byte so SendMobHeartbeat can
		// relay it to other observers instead of fabricating one.  This is
		// the value the diagnostic below has been printing all along; it was
		// decoded and discarded.  EQClassic keeps it on the Mob
		// (client_process.cpp:2373) for exactly this purpose.
		const uint64_t self_report_ms = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());

		s.trilogy_client->SetSelfReportedAnim(upd.anim_type, self_report_ms);

		// Same treatment for the turn rate.  Measured at ~3.2 reports/sec while
		// pivoting, which is far too sparse for absolute heading alone to read
		// as motion -- this is the field that carries the sweep between them.
		s.trilogy_client->SetSelfReportedDeltaHeading(upd.delta_heading, self_report_ms);

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

	LogInfo("[TrilogyZone] ChannelMessage chan={} lang={} to=[{}] msg='{}' from {}",
	        chan, lang, target, msg, s.char_name);

	// v29c sends pet orders as a tell addressed to the pet (there is no pet
	// opcode in this protocol).  Intercept those before they reach the chat
	// system, which would otherwise try to deliver them as a real tell.
	if (chan == ChatChannel_Tell && TryHandlePetCommand(s, target, msg)) {
		return;
	}

	s.trilogy_client->ChannelMessageReceived(chan, lang, lang_skill, msg, target[0] ? target : nullptr);
}

// ============================================================
// Trade window — NPC and PC-to-PC.
//
// NPC trade
// ---------
// The EQClassic client opens an NPC trade by sending OP_TradeRequest (0xd120,
// Trade_Window_Struct {int32 fromid; int32 toid;}); the server echoes
// OP_TradeAccepted (0xe620) with the ids swapped to pop the window open.  The
// player then moves items into wire slots 3000-3007 (staged via
// HandleTradeMoveItem) and may drag coins in (OP_TradeCoins 0xe420).  Clicking
// "Give" sends OP_Click_Give (0xda20); cancelling sends OP_CancelTrade (0xdb20).
//
// We do NOT route through Client::FinishTrade — Trilogy carried inventory is
// persisted DB-direct and m_inv is not kept in lock-step with client moves.
// Items dropped into the window are only RECORDED on the session (the inventory
// DB row is left in place) so an abandoned trade can never lose items.  On Give
// the rows are deleted and EVENT_TRADE fires (quest NPCs only); non-quest NPCs
// do nothing and the client returns items locally.
//
// PC-to-PC trade
// --------------
// Dropping a droppable item on another player (or right-clicking) also fires
// OP_TradeRequest (0xd120).  When the target is another TrilogyClient the
// request is FORWARDED to the partner's session (no echo) — the partner's
// client opens its own window and responds with OP_TradeAccepted (0xe620),
// which is then relayed back to the requester to open their window.
//
// Each player stages independently into wire slots 3000-3007 (and bag contents
// inline).  Each stage event triggers an OP_ItemToTrade / OP_TradeItemPacket
// (0xdf20) to the partner showing the item on their side of the window.  Each
// coin event triggers OP_TradeCoins (0xe420) to the partner.
//
// The DB is untouched until BOTH players click Give.  On the second Give the
// server runs a precheck (lore conflicts both ways + free destination slots on
// each side) and either commits the swap atomically (DELETE+INSERT per item,
// per bag content; AddMoneyToPP both ways) or aborts both sides with chat
// messages and leaves the inventory unchanged.  Cancel/disconnect mid-trade
// refunds staged coin via AddMoneyToPP (PP was debited at stage time by
// HandleMoveCoin's carried→trade path).
// ============================================================

// ============================================================
// HandleServerFilter — inbound 0xff21, Options panel filters
//
// The slot-to-filter mapping had no reference to take it from:
//
//   - EQClassic never decoded this packet.  Its struct is `int8 unknown[60]`
//     (Common/Include/eq_packet_structs.h:1622).
//   - EQEmu's SetServerFilter_Struct is uint32 filters[29] on the RoF2
//     ordering (eqFilterType, eq_constants.h:736-764).  v29c sends 15.
//
// Guessing it would silently hide real combat or chat messages — a worse
// failure than a no-op, and far harder to notice — so it was pinned by
// observation instead.
//
// MAPPING (pinned empirically 2026-08-29): the client sends one packet per
// toggle, so walking the Options panel top to bottom one click at a time
// produced one CHANGED line per option, in panel order.  Result:
//
//   idx  Options panel label      EQEmu eqFilterType
//    5   Guildchat                FilterGuildChat
//    6   Socials                  FilterSocials
//    7   Group chat               FilterGroupChat
//    8   Shouts                   FilterShouts
//    9   Auctions                 FilterAuctions
//   10   OOC                      FilterOOC
//   11   My misses                FilterMyMisses
//   12   Other misses             FilterOthersMiss
//   13   Other hits               FilterOthersHit
//   14   Attacker missing me      FilterMissedMe
//    2   PC Spells   (multi)      FilterPCSpells
//    1   NPC Spells               FilterNPCSpells
//    3   Bard songs  (multi)      FilterBardSongs
//   --   Damage shields           NOT SENT (see below)
//    0   never observed changing  unknown
//    4   never observed changing  unknown
//
// POLARITY: v29c's values pass through unchanged.  EQEmu splits filters into
// two conventions (Client::ServerFilter's Filter0 macro = 1:show/0:hide, used
// by the chat and miss filters; Filter1 = 0:show/1:hide, used by the spell
// filters), and a fresh v29c client's defaults match both -- chat filters
// default to 1, spell filters to 0, i.e. everything shown.  So the raw value
// is already in EQEmu's units for each filter it maps to.
//
// PC Spells and Bard songs are multi-state and were observed stepping 0-3.
// Their values pass through too; EQEmu reads 0=show, and the higher states are
// the narrowing ones in both.
//
// Damage shields is deliberately absent because the client does NOT send it.
// The panel walk produced 13 CHANGED lines for 14 clicks, and an isolated
// click on Damage shields alone then moved NOTHING: five packets arrived with
// all 15 slots unchanged, against a baseline byte-identical to the previous
// session's end state.  So v29c filters damage shields locally and never tells
// the server.  Slots 0 and 4 were likewise never observed moving from any
// panel toggle -- 13 of the 15 slots are all the Options panel drives.
//
// Only the filters v29c actually reports are applied, via SetFilter, rather
// than fabricating a full 29-entry SetServerFilter_Struct: the other 16
// filters have no v29c control and must keep their current values.
// ============================================================
void TrilogyZoneServer::HandleServerFilter(const std::string& addr, int port, Session& s,
                                           const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;

	static constexpr uint32_t kTrilogyFilterCount = 15; // 60 bytes / sizeof(int32)
	if (plen < kTrilogyFilterCount * sizeof(int32_t)) {
		LogInfo("[TrilogyZone] ServerFilter: char={} short packet plen={} (need {})",
		        s.char_name, plen,
		        static_cast<unsigned>(kTrilogyFilterCount * sizeof(int32_t)));
		return;
	}

	int32_t v[kTrilogyFilterCount] = {};
	memcpy(v, payload, sizeof(v));

	// v29c slot -> eqFilterType.  Damage shields is absent on purpose; see above.
	struct FilterSlot { uint32_t idx; eqFilterType type; };
	static const FilterSlot kMap[] = {
		{  5, FilterGuildChat  },
		{  6, FilterSocials    },
		{  7, FilterGroupChat  },
		{  8, FilterShouts     },
		{  9, FilterAuctions   },
		{ 10, FilterOOC        },
		{ 11, FilterMyMisses   },
		{ 12, FilterOthersMiss },
		{ 13, FilterOthersHit  },
		{ 14, FilterMissedMe   },
		{  2, FilterPCSpells   },
		{  1, FilterNPCSpells  },
		{  3, FilterBardSongs  },
	};

	for (const auto& m : kMap) {
		// Values are already in EQEmu's units for each filter (see the polarity
		// note above), so the raw value indexes eqFilterMode directly.  Clamp
		// anyway: a value outside the enum would be an out-of-range write.
		int32_t val = v[m.idx];
		if (val < 0 || val > FilterShowSelfOnly) {
			LogInfo("[TrilogyZone] ServerFilter: char={} idx={} out-of-range value {} — ignored",
			        s.char_name, m.idx, val);
			continue;
		}
		s.trilogy_client->SetFilter(m.type, static_cast<eqFilterMode>(val));
	}

	if (!s.filters_seen) {
		std::string dump;
		for (uint32_t i = 0; i < kTrilogyFilterCount; ++i) {
			dump += fmt::format("{}:{}{}", i, v[i], (i + 1 < kTrilogyFilterCount) ? " " : "");
		}
		LogInfo("[TrilogyZone] ServerFilter: char={} BASELINE [{}]", s.char_name, dump);
	}
	else {
		for (uint32_t i = 0; i < kTrilogyFilterCount; ++i) {
			if (v[i] != s.filters[i]) {
				LogInfo("[TrilogyZone] ServerFilter: char={} CHANGED idx={} {}->{}",
				        s.char_name, i, s.filters[i], v[i]);
			}
		}
	}

	memcpy(s.filters, v, sizeof(s.filters));
	s.filters_seen = true;
}

// ============================================================
// HandleWhoAll — inbound 0xf420, /who all
//
// This is /who ALL only.  A bare /who sends no packet at all: v29c assembles
// the zone roster client-side from spawn data it already has, the same way it
// builds the Tracking list, and that has always worked.  Confirmed by capture —
// /who produced nothing on the wire, /who all produced this 76-byte packet.
//
// So there is no /who-vs-/who-all ambiguity to resolve here and no need for a
// type field, which is why v29c has none and why EQClassic forwards every
// request straight to world (Zone/Source/client_process.cpp:4515,
// World/Source/ZSList.cpp:498).  Route to the server-wide path unconditionally
// and let whom[] narrow it.
//
// The forwarded struct is EQEmu's 156 B Who_All_Struct with type=3.  Handle_OP_
// WhoAllRequest routes type==0 to entity_list.ZoneWho, which builds an
// OP_ZoneEntry-based list that v29c has no handler for; type!=0 goes to
// Client::WhoAll → world → ServerOP_WhoAllReply → OP_WhoAllResponse, which is
// the path TrilogyClient::HandleOutgoingWhoAllResponse renders into chat lines.
//
// Filter fields zero-extend: every v29c "no filter" sentinel is 0xFFFF and
// EQEmu compares its uint32 fields against 0xFFFF, not 0xFFFFFFFF.
// ============================================================
void TrilogyZoneServer::HandleWhoAll(const std::string& addr, int port, Session& s,
                                     const uint8_t* payload, uint32_t plen)
{
	auto* tc = s.trilogy_client;
	if (!tc) return;

	if (plen < sizeof(Trilogy::structs::WhoAll_Struct)) {
		LogInfo("[TrilogyZone] WhoAll: char={} short packet plen={} (need {})",
		        s.char_name, plen,
		        static_cast<unsigned>(sizeof(Trilogy::structs::WhoAll_Struct)));
		return;
	}

	const auto* in = reinterpret_cast<const Trilogy::structs::WhoAll_Struct*>(payload);

	// whom[32] is not guaranteed null-terminated on the wire.
	char whom_buf[sizeof(in->whom) + 1] = {};
	memcpy(whom_buf, in->whom, sizeof(in->whom));

	// Read the filters through uint16 so the 0xFFFF sentinel survives the
	// widening (int16 -1 would sign-extend to 0xFFFFFFFF and stop matching).
	auto widen = [](int16_t raw) -> uint32 {
		return static_cast<uint32>(static_cast<uint16_t>(raw));
	};

	::Who_All_Struct out{};
	strn0cpy(out.whom, whom_buf, sizeof(out.whom));
	out.wrace    = widen(in->wrace);
	out.wclass   = widen(in->wclass);
	out.lvllow   = widen(in->firstlvl);
	out.lvlhigh  = widen(in->secondlvl);
	out.gmlookup = widen(in->gmlookup);
	out.type     = 3;          // /who all — see above

	// wguild is a real guild ID, not a filter sentinel: /who all "Fire" on a
	// server whose guild 1 is "Fire and Fury" arrives as whom="Fire" wguild=1.
	// The client resolves the name against the guild list it was handed at
	// login (TrilogyWorldServer OP_GuildsList) and sends both halves.
	//
	// Its "none" value is 0xFFFF, while EQEmu's is 0xFFFFFFFF, so this one does
	// NOT zero-extend -- map the sentinel explicitly.  Nothing on the /who all
	// path reads it (ClientList::SendWhoAll matches guilds by name from whom[],
	// and the only reader, EntityList::ZoneWho, is the type==0 branch we never
	// take), but carry it correctly rather than hardcode a lie.
	out.guildid  = (static_cast<uint16_t>(in->wguild) == 0xFFFF)
		? 0xFFFFFFFF
		: static_cast<uint32>(static_cast<uint16_t>(in->wguild));

	// Logged in full: the filter fields are the part with no reference to check
	// against.  wguild in particular is a slot EQClassic does not declare, and
	// only /who all <guildname> will show what the client puts there.
	LogInfo("[TrilogyZone] WhoAll: char={} whom='{}' race={:#x} class={:#x} "
	        "lvl={:#x}-{:#x} gm={:#x} guild={:#x}",
	        s.char_name, std::string(whom_buf), out.wrace, out.wclass,
	        out.lvllow, out.lvlhigh, out.gmlookup,
	        static_cast<uint32>(static_cast<uint16_t>(in->wguild)));

	auto* app = new EQApplicationPacket(OP_WhoAllRequest, sizeof(::Who_All_Struct));
	memcpy(app->pBuffer, &out, sizeof(out));
	tc->Handle_OP_WhoAllRequest(app);
	delete app;
}

// ============================================================
// HandleZoneEntryResend — inbound 0x4121, "send me this spawn again"
//
// See the constant for what the traffic looks like on the wire.  Three cases:
//
//   1. The mob still exists and this session should see it -> rebuild its spawn
//      packet and push it through TrilogyClient::QueuePacket.  That lands in
//      HandleNewSpawn, which already routes players / bots / Playerbots to
//      0x6121 (zone-permanent, never staled by the client) and regular NPCs to
//      0x4921, so the resend inherits the correct treatment per mob type for
//      free.  known_spawns is refreshed so the ghost reconciler and the
//      position-broadcast cache stay in step.
//
//   2. The mob is gone server-side -> answer with DeleteSpawn instead.  The
//      client is holding a reference we can never satisfy, and the original
//      delete evidently did not stick; this is the same repair the ghost
//      reconciler performs on its own schedule, just triggered by the client.
//
//   3. The id is this client's own synthetic player_spawn_id -> ignore.  Echoing
//      a player their own spawn is what HandleNewSpawn already refuses to do.
//
// Answering is what stops the sweep: the retries exist only because nothing
// came back.  The per-spawn cooldown below is not for the normal case, it is a
// backstop against a request we can never satisfy turning into a hot loop.
// ============================================================
void TrilogyZoneServer::HandleZoneEntryResend(const std::string& addr, int port, Session& s,
                                              const uint8_t* payload, uint32_t plen)
{
	auto* tc = s.trilogy_client;
	if (!tc) return;

	if (plen < sizeof(int16_t)) {
		LogInfo("[TrilogyZone] ZoneEntryResend: char={} short packet plen={}",
		        s.char_name, plen);
		return;
	}

	int16_t raw = 0;
	memcpy(&raw, payload, sizeof(raw));
	const uint16_t spawn_id = static_cast<uint16_t>(raw);

	// The client refers to itself by the synthetic player_spawn_id.
	if (spawn_id == s.player_spawn_id) return;

	// Backstop only.  One second is far longer than the client's own retry
	// cadence (the observed sweep repeated within ~1s), so a request we answer
	// correctly is never throttled -- only one we keep failing to satisfy.
	static constexpr uint64_t kResendCooldownMs = 1000;
	const uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	auto it = s.last_resend_ms.find(spawn_id);
	if (it != s.last_resend_ms.end() && now_ms - it->second < kResendCooldownMs) {
		return;
	}
	s.last_resend_ms[spawn_id] = now_ms;

	Mob* mob = entity_list.GetMob(spawn_id);
	if (!mob) {
		Trilogy::structs::DeleteSpawn_Struct ds{};
		ds.spawn_id    = raw;
		ds.ds_unknown1 = 0;
		SendToSession(SessionKey(addr, port), 0x2b20,
		              reinterpret_cast<const uint8_t*>(&ds),
		              static_cast<uint32_t>(sizeof(ds)));
		s.known_spawns.erase(spawn_id);
		s.last_broadcast.erase(spawn_id);
		s.last_desync_log_ms.erase(spawn_id);
		LogInfo("[TrilogyZone] ZoneEntryResend: char={} sid={} GONE — sent 2B20 delete",
		        s.char_name, spawn_id);
		return;
	}

	// QueuePacket lands in TrilogyClient::HandleNewSpawn, which picks 0x6121 vs
	// 0x4921 by mob type and registers the spawn in known_spawns itself, so the
	// resend is indistinguishable from any other spawn send.
	EQApplicationPacket app;
	mob->CreateSpawnPacket(&app, tc);
	tc->QueuePacket(&app);

	LogInfo("[TrilogyZone] ZoneEntryResend: char={} sid={} name='{}' type={} "
	        "pos=({:.1f},{:.1f},{:.1f}) — resent",
	        s.char_name, spawn_id, mob->GetCleanName(),
	        mob->IsClient() ? "client" : (mob->IsCorpse() ? "corpse"
	                       : (mob->IsBot() ? "bot" : "npc")),
	        mob->GetX(), mob->GetY(), mob->GetZ());
}

// ============================================================
// HandleSetRunMode — inbound 0x1f20, walk/run toggle
//
// The Trilogy wire struct and EQEmu's SetRunMode_Struct are the same four
// bytes, so this is a straight memcpy into the existing handler.  See the
// constant for why it matters: without it Client::runmode is stuck false for
// the whole session and the server models every v29c player as walking.
// ============================================================
void TrilogyZoneServer::HandleSetRunMode(const std::string& addr, int port, Session& s,
                                         const uint8_t* payload, uint32_t plen)
{
	auto* tc = s.trilogy_client;
	if (!tc) return;

	if (plen < sizeof(::SetRunMode_Struct)) {
		LogInfo("[TrilogyZone] SetRunMode: char={} short packet plen={} (need {})",
		        s.char_name, plen,
		        static_cast<unsigned>(sizeof(::SetRunMode_Struct)));
		return;
	}

	auto* app = new EQApplicationPacket(OP_SetRunMode, sizeof(::SetRunMode_Struct));
	memcpy(app->pBuffer, payload, sizeof(::SetRunMode_Struct));
	tc->Handle_OP_SetRunMode(app);
	delete app;

	LogInfo("[TrilogyZone] SetRunMode: char={} mode={}",
	        s.char_name,
	        reinterpret_cast<const ::SetRunMode_Struct*>(payload)->mode ? "run" : "walk");
}

// ============================================================
// HandleClientError — inbound 0x4721, the client reporting its own fault
//
// Logged, not parsed: neither reference tree declares a struct for it.  The
// leading two int32s are constant across every capture and the third is an
// error code; everything after that is code-dependent and reads like a fault
// report with a return address, which needs the client binary to decode.  See
// the constant for the full breakdown.
//
// The whole payload goes to the log because the point of consuming this opcode
// is to stop throwing the signal away, and a truncated dump would defeat that.
// It is rare — single digits per session — so there is no volume concern.
// ============================================================
void TrilogyZoneServer::HandleClientError(const std::string& addr, int port, Session& s,
                                          const uint8_t* payload, uint32_t plen)
{
	uint32_t code = 0;
	if (plen >= 12) {
		memcpy(&code, payload + 8, sizeof(code));
	}

	std::string hex;
	hex.reserve(plen * 3);
	for (uint32_t i = 0; i < plen; ++i) {
		hex += fmt::format("{:02X} ", payload[i]);
	}

	LogInfo("[TrilogyZone] ClientError: char={} code={} plen={} payload=[{}]",
	        s.char_name, code, plen, hex);
}

// ============================================================
// HandleSocialCommand — /assist /random /split /yell /lfg /consent
//
// Six commands that shared one cause: the opcode was never dispatched, so the
// client sent a well-formed packet into the unhandled logger.  EQEmu already
// implements every one of them; only the Trilogy inbound branch was missing.
//
// Wire sizes are measured, not inferred (see the opcode block at the top of this
// file).  Every struct except LFG is byte-identical to its modern counterpart —
// EQClassic's Split_Struct, Random_Struct and ConsentRequest_Struct match
// EQEmu's field for field — so these are a memcpy plus a dispatch.  Sizes are
// checked with >= rather than == because v29c pads some of these.
// ============================================================
void TrilogyZoneServer::HandleSocialCommand(const std::string& addr, int port, Session& s,
                                            uint16_t opcode, const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	TrilogyClient* tc = s.trilogy_client;

	auto too_short = [&](uint32_t need, const char* what) -> bool {
		if (plen >= need) return false;
		LogInfo("[TrilogyZone] Social: char={} {} short packet plen={} (need {})",
		        s.char_name, what, plen, need);
		return true;
	};

	switch (opcode) {

	case ZN_OP_Assist: {
		if (too_short(sizeof(::EntityId_Struct), "/assist")) return;
		auto* app = new EQApplicationPacket(OP_Assist, sizeof(::EntityId_Struct));
		memcpy(app->pBuffer, payload, sizeof(::EntityId_Struct));
		LogInfo("[TrilogyZone] Social: char={} /assist entity={}",
		        s.char_name, reinterpret_cast<const ::EntityId_Struct*>(payload)->entity_id);
		tc->Handle_OP_Assist(app);
		delete app;
		break;
	}

	case ZN_OP_Random: {
		if (too_short(sizeof(::RandomReq_Struct), "/random")) return;
		auto* app = new EQApplicationPacket(OP_RandomReq, sizeof(::RandomReq_Struct));
		memcpy(app->pBuffer, payload, sizeof(::RandomReq_Struct));
		const auto* rr = reinterpret_cast<const ::RandomReq_Struct*>(payload);
		LogInfo("[TrilogyZone] Social: char={} /random {}-{}", s.char_name, rr->low, rr->high);
		tc->Handle_OP_RandomReq(app);
		delete app;
		break;
	}

	case ZN_OP_SplitMoney: {
		if (too_short(sizeof(::Split_Struct), "/split")) return;
		auto* app = new EQApplicationPacket(OP_Split, sizeof(::Split_Struct));
		memcpy(app->pBuffer, payload, sizeof(::Split_Struct));
		const auto* sp = reinterpret_cast<const ::Split_Struct*>(payload);
		LogInfo("[TrilogyZone] Social: char={} /split {}p {}g {}s {}c",
		        s.char_name, sp->platinum, sp->gold, sp->silver, sp->copper);
		tc->Handle_OP_Split(app);
		delete app;
		break;
	}

	case ZN_OP_Yell: {
		// Payload is the yeller's own id, which we do not need: EQEmu's handler
		// builds its own outbound from GetID() and broadcasts to close clients.
		LogInfo("[TrilogyZone] Social: char={} /yell", s.char_name);
		EQApplicationPacket app(OP_YellForHelp, 0);
		tc->Handle_OP_YellForHelp(&app);
		break;
	}

	case ZN_OP_LFG: {
		// The one struct that is NOT shared.  Trilogy sends
		// { char name[32]; int32 value } (EQClassic eq_packet_structs.h:1288,
		// PC_MAX_NAME_LENGTH + 2), against EQEmu's larger LFG_Struct with match
		// filter and level range.  Only the on/off flag carries over; the rest
		// stay zeroed, which is correct for an era with no LFG filters.
		if (too_short(36, "/lfg")) return;
		int32_t value = 0;
		memcpy(&value, payload + 32, sizeof(value));

		// Probe: does the client populate the name field on the way out, or
		// leave it for the server?  Decides whether 0xf021 is meaningful in the
		// server->client direction at all -- an echo carrying a name the client
		// never uses is a different problem from one whose layout is wrong.
		{
			char sent_name[33] = {};
			memcpy(sent_name, payload, 32);
			std::string hex;
			for (uint32_t i = 0; i < 36 && i < plen; ++i)
				hex += fmt::format("{:02X} ", payload[i]);
			LogInfo("[TrilogyLFG] inbound | char=[{}] name_field=['{}'] value={} raw=[{}]",
			        s.char_name, sent_name, value, hex);
		}

		auto* app = new EQApplicationPacket(OP_LFGCommand, sizeof(::LFG_Struct));
		auto* lfg = reinterpret_cast<::LFG_Struct*>(app->pBuffer);
		memset(lfg, 0, sizeof(::LFG_Struct));
		lfg->value = static_cast<uint8>(value ? 1 : 0);
		LogInfo("[TrilogyZone] Social: char={} /lfg {}", s.char_name, value ? "on" : "off");
		tc->Handle_OP_LFGCommand(app);
		delete app;

		// ── LFG channel search: what has been ruled out ─────────────────────
		// All three by measurement, not inference:
		//
		//   0xf021 echo — the client fills name@0 / value@32 outbound and
		//     ignores that exact layout inbound, so the opcode is one-way.
		//   SpawnAppearance — every unexplained type in the enum (2, 6-13)
		//     swept with param=1; nothing rendered on the observer.
		//   Spawn_Struct unknowns 078/084/087/090 — set together on a re-sent
		//     spawn.  The re-send visibly re-placed the entity client-side, so
		//     the packet was parsed; the bytes did nothing.
		//
		// The spawn re-send is NOT repeated here: re-issuing a 0x6121 for a
		// live entity makes v29c re-place it, which renders as the character
		// dropping out of the sky.  Any further spawn-byte probing has to ride
		// an actual zone-in rather than a mid-session re-send.
		//
		// Remaining surface is /who all, which unlike a bare /who does reach
		// the server (bare /who is client-side — the roster is built from the
		// client's own spawn list, confirmed by zero WhoAll log lines while
		// testing).  That path is rendered by
		// TrilogyClient::HandleOutgoingWhoAllResponse, where the LFG marker is
		// now appended.


		// No hand-rolled relay here any more.  Handle_OP_LFGCommand above
		// already does both halves of the job: UpdateWho() pushes the flag to
		// world (ServerClientList_Struct::LFG, servertalk.h:600) so /who can
		// report it, and it broadcasts OP_LFGAppearance {spawn_id, lfg} to
		// every other client in the zone.  That broadcast reaches Trilogy
		// observers through TranslateAndSend -> HandleLFGAppearance, which is
		// where the wire packet is now built.
		//
		// The previous loop forwarded this client's own inbound bytes to every
		// other session.  It never worked, and it could not: the payload is
		// whatever the SENDER's client put in the name field, forwarded
		// unchanged, so a recipient had no reliable way to know which nameplate
		// the flag belonged to.  Driving it off the entity id in
		// OP_LFGAppearance removes that ambiguity and means LFG set by any
		// other path is relayed too, not just a typed /lfg.
		break;
	}

	case ZN_OP_ConsentRequest: {
		// Variable-length null-terminated name.  Handle_OP_Consent requires
		// size < 64 and reads Consent_Struct::name, which is a char[1] the
		// handler walks as a C string, so send name + terminator and nothing more.
		// Cap at 62 chars so name + terminator can never reach 64 — the handler
		// guards on `size < 64` and would silently drop a longer one.
		char name[63] = {};
		const uint32_t cap = plen < sizeof(name) ? plen : static_cast<uint32_t>(sizeof(name) - 1);
		memcpy(name, payload, cap);
		name[sizeof(name) - 1] = '\0';
		if (!name[0]) {
			LogInfo("[TrilogyZone] Social: char={} /consent with empty name - ignored", s.char_name);
			return;
		}
		const uint32_t len = static_cast<uint32_t>(strlen(name)) + 1;
		auto* app = new EQApplicationPacket(OP_Consent, len);
		memcpy(app->pBuffer, name, len);
		LogInfo("[TrilogyZone] Social: char={} /consent [{}]", s.char_name, name);
		tc->Handle_OP_Consent(app);
		delete app;
		break;
	}

	default:
		break;
	}
}


// ============================================================
// SendGuildsList — 0x9221, the client's guild-name table
//
// World sends this once at char-select and the client keeps it for the life of
// the process, so a guild created or renamed mid-session has no name on any
// client already in a zone.  The client has its own repair channel for exactly
// that: it sends 0x2821 carrying the guild id it cannot resolve (observed
// 2026-09-05, payload 65 01 00 00 = 357, the guild that had just been created)
// and waits for the table.  Same shape as the 0x4121 spawn-resend sweep.
//
// Byte-identical to TrilogyWorldServer::SendGuildsList — both fill slots
// through Trilogy::structs::FillGuildsListEntry so the two cannot drift.
// 30724 bytes, which SendApp fragments into ~60 datagrams.
// ============================================================
void TrilogyZoneServer::SendGuildsList(uint64_t session_key)
{
	static constexpr uint64_t kGuildsListMinIntervalMs = 30000;

	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	Session& s = it->second;

	const uint64_t now_ms = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::now().time_since_epoch()).count());
	if (s.last_guilds_list_ms != 0 &&
	    now_ms - s.last_guilds_list_ms < kGuildsListMinIntervalMs) {
		LogInfo("[TrilogyGuild] guilds table for char={} suppressed ({} ms since last)",
		        s.char_name, now_ms - s.last_guilds_list_ms);
		return;
	}
	s.last_guilds_list_ms = now_ms;

	auto gl = std::make_unique<Trilogy::structs::GuildsList_Struct>();
	memset(gl.get(), 0, sizeof(*gl));
	for (uint32_t i = 0; i < Trilogy::structs::MAX_GUILDS; ++i) {
		Trilogy::structs::FillGuildsListEntry(gl->Guilds[i], 0, nullptr);
	}

	auto     guilds    = guild_mgr.MakeGuildList();
	uint32_t populated = 0;
	uint32_t skipped   = 0;
	for (auto const& entry : guilds.guild_detail) {
		// The spawn struct's GuildID is a uint16 and the client's table is
		// bounded at 512, so anything past that can never be rendered.
		if (entry.guild_id >= Trilogy::structs::MAX_GUILDS) {
			++skipped;
			continue;
		}
		Trilogy::structs::FillGuildsListEntry(gl->Guilds[entry.guild_id],
		                                      entry.guild_id,
		                                      entry.guild_name.c_str());
		++populated;
	}

	LogInfo("[TrilogyGuild] OUT guilds table char={} ({} guild(s), {} out of range, {} bytes)",
	        s.char_name, populated, skipped,
	        static_cast<uint32_t>(sizeof(*gl)));

	SendToSession(session_key, 0x9221,
	              reinterpret_cast<const uint8_t*>(gl.get()),
	              static_cast<uint32_t>(sizeof(*gl)));
}

// ============================================================
// HandleGuildCommand — the whole guild command family
//
// Nothing in this family was dispatched, so a guild could only be created and
// populated by GM command or direct SQL.  Every piece of the logic already
// exists in guild_mgr and the Handle_OP_Guild* family; the Trilogy inbound
// branch was the only missing part, exactly like the social batch in #38.
//
// Two things about this family need care:
//
//   Name field order.  EQClassic labels the leading name field "Invitee" in
//   one struct and "Remover" in another, for what is the same 68-byte packet.
//   Rather than pick one and find out in play, every handler below identifies
//   the sender by matching either name against the session's own character and
//   takes the other as the target.  That is correct under both readings.
//
//   The refusal codes.  v29c answers an invite with a GUILDRANK value: 0/1/2
//   accept at that rank, 4 = the popup timed out, 5 = declined.  It cannot be
//   forwarded raw: Client::Handle_OP_GuildInviteAccept converts pre-RoF ranks
//   with a switch whose default arm maps anything unrecognised to
//   GUILD_RANK_NONE (0), and its decline test is `response >= 9`.  A decline
//   would therefore arrive as an acceptance at rank 0.  Refusals are answered
//   here instead and never reach that handler.
// ============================================================

// Read a fixed-width v29c name field that may not be null-terminated.
static std::string TrilogyGuildName(const char* field, size_t width)
{
	size_t len = 0;
	while (len < width && field[len] != '\0') ++len;
	return std::string(field, len);
}

static bool TrilogyNameIs(const std::string& a, const char* b)
{
	return b != nullptr && strcasecmp(a.c_str(), b) == 0;
}

void TrilogyZoneServer::HandleGuildCommand(const std::string& addr, int port, Session& s,
                                           uint16_t opcode, const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	TrilogyClient* tc = s.trilogy_client;

	// Unconditional payload dump.  No packet in this family has ever been seen
	// on this branch and there are no captures, so the offsets below come from
	// EQClassic's headers rather than from observation.  The first live run of
	// each command is the only chance to confirm them, and a hex line costs
	// nothing at the rate a player types guild commands.
	{
		std::string hex;
		const uint32_t cap = plen > 96 ? 96u : plen;
		hex.reserve(cap * 3);
		for (uint32_t i = 0; i < cap; ++i) {
			hex += fmt::format("{:02X} ", payload[i]);
		}
		if (plen > cap) hex += "...";
		LogInfo("[TrilogyGuild] rx opcode={:04X} char={} plen={} payload=[{}]",
		        opcode, s.char_name, plen, hex);
	}

	// GetName(), not GetCleanName(): every Handle_OP_Guild* below compares the
	// name field against GetName(), and the self-remove path in
	// Handle_OP_GuildRemove is an exact-match test on it.
	const char* self = tc->GetName();

	auto too_short = [&](uint32_t need, const char* what) -> bool {
		if (plen >= need) return false;
		LogInfo("[TrilogyGuild] char={} {} short packet plen={} (need {})",
		        s.char_name, what, plen, need);
		return true;
	};

	switch (opcode) {

	// ------------------------------------------------------------------
	// /guildinvite and /guildremove share one 68-byte struct.  Both modern
	// handlers take the 136-byte EQEmu GuildCommand_Struct, so this is a
	// name-and-rank copy into the wider struct plus a dispatch.
	//
	// The rank byte is left on the v29c scale deliberately: the modern
	// handler already converts GUILD_MEMBER_TI / OFFICER_TI / LEADER_TI for
	// any client below RoF, and Trilogy's 0/1/2 are exactly those values.
	// ------------------------------------------------------------------
	case ZN_OP_GuildInvite:
	case ZN_OP_GuildRemove: {
		const bool is_invite = (opcode == ZN_OP_GuildInvite);
		const char* what = is_invite ? "/guildinvite" : "/guildremove";
		if (too_short(sizeof(Trilogy::structs::GuildCommand_Struct), what)) return;

		const auto* in =
		    reinterpret_cast<const Trilogy::structs::GuildCommand_Struct*>(payload);
		const std::string first  = TrilogyGuildName(in->othername, sizeof(in->othername));
		const std::string second = TrilogyGuildName(in->myname,    sizeof(in->myname));

		// Whichever field is not us is the target.  A self-remove (leaving your
		// own guild) puts our own name in both, which still resolves to us.
		std::string target = first;
		if (TrilogyNameIs(first, self) && !TrilogyNameIs(second, self)) {
			target = second;
		}

		LogInfo("[TrilogyGuild] char={} {} target=[{}] fields=[{}|{}] guildeqid={} rank={}",
		        s.char_name, what, target, first, second,
		        static_cast<int>(in->guildeqid), static_cast<int>(in->rank));

		if (target.empty()) {
			tc->Message(Chat::Red, "You must name a player.");
			return;
		}

		EQApplicationPacket app(is_invite ? OP_GuildInvite : OP_GuildRemove,
		                        sizeof(::GuildCommand_Struct));
		// EQApplicationPacket zeroes the buffer it allocates.
		auto* gc = reinterpret_cast<::GuildCommand_Struct*>(app.pBuffer);
		strn0cpy(gc->othername, target.c_str(), sizeof(gc->othername));
		strn0cpy(gc->myname,    self,           sizeof(gc->myname));
		gc->guildeqid = static_cast<uint16>(in->guildeqid);
		gc->officer   = static_cast<uint32>(in->rank);

		if (is_invite) {
			tc->Handle_OP_GuildInvite(&app);
		} else {
			tc->Handle_OP_GuildRemove(&app);
		}
		break;
	}

	// ------------------------------------------------------------------
	// Answer to the invite popup.  Layout differs from the invite itself:
	// the inviter comes first here, the new member second.
	// ------------------------------------------------------------------
	case ZN_OP_GuildInviteAccept: {
		if (too_short(sizeof(Trilogy::structs::GuildInviteAccept_Struct), "guild invite answer"))
			return;

		const auto* in =
		    reinterpret_cast<const Trilogy::structs::GuildInviteAccept_Struct*>(payload);
		const std::string first  = TrilogyGuildName(in->inviter,    sizeof(in->inviter));
		const std::string second = TrilogyGuildName(in->new_member, sizeof(in->new_member));

		// We are the new member; the other field names the inviter.
		std::string inviter = first;
		if (TrilogyNameIs(first, self) && !TrilogyNameIs(second, self)) {
			inviter = second;
		}

		// Trust the inviter's own guild over the wire value: v29c echoes a
		// guild id here whose meaning is unconfirmed, and getting it wrong
		// would put the player into whatever guild that number names.
		Client* inviter_client = inviter.empty() ? nullptr
		                                         : entity_list.GetClientByName(inviter.c_str());
		const uint32 wire_guild  = static_cast<uint32>(in->guildeqid);
		const uint32 guild_id    = inviter_client ? inviter_client->GuildID() : wire_guild;

		LogInfo("[TrilogyGuild] char={} invite answer inviter=[{}] fields=[{}|{}] "
		        "response={} wire_guild={} resolved_guild={}",
		        s.char_name, inviter, first, second,
		        static_cast<int>(in->response), wire_guild, guild_id);

		// Anything outside the three in-guild ranks is a refusal — see the
		// header comment for why this cannot be forwarded.
		if (in->response < Trilogy::structs::GUILDRANK_MEMBER ||
		    in->response > Trilogy::structs::GUILDRANK_LEADER) {
			tc->SetPendingGuildInvitation(false);
			guild_mgr.VerifyAndClearInvite(tc->CharacterID(), guild_id,
			                               static_cast<uint8>(in->response));
			const bool timed_out = (in->response == 4); // GuildInviteTimeOut
			tc->Message(Chat::Yellow, timed_out ? "The guild invite timed out."
			                                    : "You declined the guild invite.");
			if (inviter_client) {
				inviter_client->Message(Chat::Yellow,
				                        timed_out ? "%s's guild invite window timed out."
				                                  : "%s has declined to join the guild.",
				                        self);
			}
			return;
		}

		EQApplicationPacket app(OP_GuildInviteAccept, sizeof(::GuildInviteAccept_Struct));
		auto* gia = reinterpret_cast<::GuildInviteAccept_Struct*>(app.pBuffer);
		strn0cpy(gia->inviter,    inviter.c_str(), sizeof(gia->inviter));
		strn0cpy(gia->new_member, self,            sizeof(gia->new_member));
		gia->response = static_cast<uint32>(in->response);
		gia->guild_id = guild_id;
		tc->Handle_OP_GuildInviteAccept(&app);
		break;
	}

	// ------------------------------------------------------------------
	// /guildmotd, both directions on one opcode.
	//
	// There is no verified struct for this.  EQClassic's zone handler is a
	// dead stub (it returns on a guilddbid it never assigns) and its LS
	// ancestor reads the text at offset 68, which is name[64] + 4 on a
	// client whose names are 64 bytes wide — not this one.  So rather than
	// pick an offset, walk the payload for null-terminated printable strings.
	// The client sends its own name first and the new MOTD in the
	// "Name - text" form the LS handler documents, with "Name - none" as the
	// clear.  A packet with no text is the request form, which v29c also
	// sends once at login even for a player with no guild.
	// ------------------------------------------------------------------
	case ZN_OP_GuildMOTD: {
		// Only the fixed head is required.  The client allocates the full 548
		// bytes but there is no guarantee it sends all of them, and the text is
		// read bounded by what actually arrived rather than by the struct.
		constexpr uint32_t kMOTDTextOffset =
		    offsetof(Trilogy::structs::GuildMOTD_Struct, motd);
		if (too_short(kMOTDTextOffset, "/guildmotd")) return;

		const auto* in =
		    reinterpret_cast<const Trilogy::structs::GuildMOTD_Struct*>(payload);
		const std::string setter = TrilogyGuildName(in->name, sizeof(in->name));
		const uint32_t    text_avail =
		    std::min<uint32_t>(plen - kMOTDTextOffset, sizeof(in->motd));
		std::string motd = TrilogyGuildName(in->motd, text_avail);

		LogInfo("[TrilogyGuild] char={} /guildmotd setter=[{}] wire_guild={} text=[{}]",
		        s.char_name, setter, in->guildid, motd);

		// The client sends the text as "<Name> - <text>", and "<Name> - none"
		// is its documented clear form.  Empty means it is asking, not setting.
		const bool is_set = !motd.empty();
		if (is_set) {
			const size_t sep = motd.find(" - ");
			if (sep != std::string::npos &&
			    (TrilogyNameIs(motd.substr(0, sep), self) ||
			     strcasecmp(motd.substr(0, sep).c_str(), setter.c_str()) == 0)) {
				motd = motd.substr(sep + 3);
			}
			if (strcasecmp(motd.c_str(), "none") == 0) {
				motd.clear();
			}
		}

		if (!tc->IsInAGuild()) {
			// v29c polls this at login regardless of guild membership; only
			// say something if the player actually tried to set one.
			if (is_set) {
				tc->Message(Chat::Red, "You are not a member of any guild.");
			}
			return;
		}

		if (!is_set) {
			// Request form, and it is always answered.  v29c asks for its MOTD
			// once on its own immediately after zone-in — confirmed on the wire,
			// and what EQClassic documents — and sends the identical packet when
			// the player types a bare /guildmotd.  Nothing in the packet tells
			// the two apart.
			//
			// So the login display is driven entirely by the client's own poll:
			// HandleOutgoingGuildMOTD drops the copy CompleteConnect emits during
			// zone-in, which is what stops the two printing the same line twice.
			// That leaves every request here free to force a display, which is
			// the only behaviour that is right for a command the player typed.
			//
			// An earlier attempt treated the first request of a session as the
			// poll.  It had a hole: the client only polls when it already
			// believes it is in a guild (the gate at eqgame.exe 0x4a54c8 rejects
			// 0xFFFF before sending), so a player who logs in unguilded and joins
			// mid-session never sends one — and their first typed /guildmotd was
			// swallowed as though it were the poll.
			char cur[512]  = {};
			char setby[64] = {};
			if (guild_mgr.GetGuildMOTD(tc->GuildID(), cur, setby) && cur[0]) {
				tc->SendGuildMOTD(true); // OP_GetGuildMOTDReply — always displayed
			} else if (!tc->IsZoning()) {
				// Nothing to send: the outbound path drops an empty MOTD, so say
				// it here or a player who types /guildmotd on a guild that has
				// none set gets silence and reads it as a broken command.
				//
				// Skipped for the automatic poll, which is the one request that
				// is reliably distinguishable: it arrives before the client's
				// first position update, and a player cannot type until after
				// their world is up.  Otherwise every zone-in on a guild with no
				// MOTD would open with a line nobody asked for.
				tc->Message(Chat::Guild, "Your guild has no message of the day set.");
			}
			return;
		}

		if (!guild_mgr.CheckPermission(tc->GuildID(), tc->GuildRank(),
		                               GUILD_ACTION_CHANGE_THE_MOTD)) {
			tc->Message(Chat::Red, "You do not have permission to set the guild message of the day.");
			return;
		}

		LogInfo("[TrilogyGuild] char={} setting guild {} MOTD to [{}]",
		        s.char_name, tc->GuildID(), motd);

		if (!guild_mgr.SetGuildMOTD(tc->GuildID(), motd, self)) {
			tc->Message(Chat::Red, "Failed to update the guild message of the day.");
		}
		// The confirmation to the whole guild rides the refresh guild_mgr
		// pushes through world, which lands back here as SendGuildMOTD.
		break;
	}

	// ------------------------------------------------------------------
	// /guildleader <name> — bare null-terminated name, no struct.
	// ------------------------------------------------------------------
	case ZN_OP_GuildLeader: {
		char new_leader[64] = {};
		const uint32_t cap = plen < sizeof(new_leader) ? plen
		                                               : static_cast<uint32_t>(sizeof(new_leader) - 1);
		memcpy(new_leader, payload, cap);
		new_leader[sizeof(new_leader) - 1] = '\0';

		LogInfo("[TrilogyGuild] char={} /guildleader [{}]", s.char_name, new_leader);

		if (!new_leader[0]) {
			tc->Message(Chat::Red, "You must name the new guild leader.");
			return;
		}

		EQApplicationPacket app(OP_GuildLeader, sizeof(::GuildMakeLeader_Struct));
		auto* gml = reinterpret_cast<::GuildMakeLeader_Struct*>(app.pBuffer);
		strn0cpy(gml->requestor,  self,       sizeof(gml->requestor));
		strn0cpy(gml->new_leader, new_leader, sizeof(gml->new_leader));
		tc->Handle_OP_GuildLeader(&app);
		break;
	}

	// ------------------------------------------------------------------
	// /guilddelete — the modern handler reads nothing from the payload and
	// does the leader check itself.
	// ------------------------------------------------------------------
	case ZN_OP_GuildDelete: {
		LogInfo("[TrilogyGuild] char={} /guilddelete guild={}", s.char_name, tc->GuildID());
		EQApplicationPacket app(OP_GuildDelete, 0);
		tc->Handle_OP_GuildDelete(&app);
		break;
	}

	// ------------------------------------------------------------------
	// The client asking for the guild-name table, carrying the id it could
	// not resolve.  This is its own repair channel for a guild that was
	// created after it received its char-select table.
	// ------------------------------------------------------------------
	case ZN_OP_GetGuildsList: {
		uint32_t wanted = 0;
		if (plen >= sizeof(wanted)) memcpy(&wanted, payload, sizeof(wanted));
		LogInfo("[TrilogyGuild] char={} requested guilds table (unresolved guild id {})",
		        s.char_name, wanted);
		SendGuildsList(SessionKey(addr, port));
		break;
	}

	// ------------------------------------------------------------------
	// Guild wars never shipped in this era.  EQClassic answers the same way
	// rather than leaving the command silent, and a silent command reads as
	// a broken client.
	// ------------------------------------------------------------------
	case ZN_OP_GuildWar:
	case ZN_OP_GuildPeace: {
		LogInfo("[TrilogyGuild] char={} {} - not implemented",
		        s.char_name, opcode == ZN_OP_GuildWar ? "/guildwar" : "/guildpeace");
		tc->Message(Chat::Red, "Guild wars are not implemented on this server.");
		break;
	}

	default:
		break;
	}
}

// ============================================================
// HandleBuffCancel — inbound 0x3221, player clicked a buff off
//
// 0x3221 is bidirectional.  Outbound we use it for duration refreshes and the
// buff bar; inbound it is the client asking to drop a buff, which is what
// right-clicking a buff icon sends.  Nothing consumed the inbound direction, so
// buffs could not be removed at all — and an icon that did disappear left the
// client and server disagreeing, because the server never learned.
//
// Wire layout is the 20-byte Trilogy Buff_Struct, which maps onto the first 20
// bytes of EQClassic's SpellBuffFade_Struct (eq_packet_structs.h:349).  Observed:
//
//   12 40 00 00  00 00 00 00  CF 06 ...   -> spell 1743, drop it
//   12 40 00 00  00 00 00 00  FF FF ...   -> 0xFFFF, the "no spell" sentinel
//
// Translate and dispatch into Client::Handle_OP_Buff rather than calling
// BuffFadeBySpellID directly: that handler owns the rule that DETRIMENTAL
// spells cannot be clicked off, and refuses them by echoing the packet back
// (client_packet.cpp:4249).  EQClassic's ProcessOP_Buff has no such check and
// fades anything, which would let a player click off their own debuffs.
// ============================================================
void TrilogyZoneServer::HandleBuffCancel(const std::string& addr, int port, Session& s,
                                         const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::Buff_Struct)) {
		LogInfo("[TrilogyZone] BuffCancel: char={} short packet plen={} (need {})",
		        s.char_name, plen,
		        static_cast<unsigned>(sizeof(Trilogy::structs::Buff_Struct)));
		return;
	}

	const auto* tri = reinterpret_cast<const Trilogy::structs::Buff_Struct*>(payload);

	// 0xFFFF is the client's "no spell" sentinel.  Handle_OP_Buff treats it the
	// same way it treats a detrimental spell (echo, do not fade), so pass it
	// through rather than filtering here and diverging from that rule.
	const uint16 spell_id = static_cast<uint16>(tri->spell_id);

	LogInfo("[TrilogyZone] BuffCancel: char={} spell_id={} buff_slot={}",
	        s.char_name, spell_id, tri->buff_slot);

	auto* app = new EQApplicationPacket(OP_Buff, sizeof(::SpellBuffPacket_Struct));
	auto* emu = reinterpret_cast<::SpellBuffPacket_Struct*>(app->pBuffer);
	memset(emu, 0, sizeof(::SpellBuffPacket_Struct));

	emu->entityid      = static_cast<uint32>(s.trilogy_client->GetID());
	emu->buff.spellid  = static_cast<uint32>(spell_id);
	emu->slotid        = static_cast<uint32>(tri->buff_slot);
	emu->bufffade      = 1;

	s.trilogy_client->Handle_OP_Buff(app);
	delete app;
}

// ============================================================
// TryHandlePetCommand — pet orders arrive as tells, not an opcode
//
// v29c has no pet opcode at all.  Typing `/pet attack` makes the CLIENT emit a
// TELL addressed to the pet's name, with the order as the message body — the
// same mechanism EQClassic parses in its tell branch
// (Zone/Source/Client_Messaging.cpp:199-330).  Captured from a live session:
//
//     ChannelMessage chan=7 msg='attack Al`eik_K`vorr000'
//     ChannelMessage chan=7 msg='guard'
//     ChannelMessage chan=7 msg='sit'
//
// Note that `attack` appends the current target's name.  We therefore match on
// a leading-keyword PREFIX rather than EQClassic's strstr over the whole
// string: a target called "a_guard000" makes "attack a_guard000" contain both
// ATTACK and GUARD, and substring order is a fragile way to disambiguate that.
//
// Rather than reimplement the orders, translate to the modern PetCommand_Struct
// and dispatch into Client::Handle_OP_PetCommands, which already owns hate-list
// handling, pet-order state, hold/focus semantics and the response messages.
// ============================================================
bool TrilogyZoneServer::TryHandlePetCommand(Session& s, const char* targetname, const char* msg)
{
	if (!s.trilogy_client || !msg || !*msg) return false;

	Mob* mypet = s.trilogy_client->GetPet();
	if (!mypet) return false;

	// Is this tell addressed to our own pet?  Be lenient about the exact form:
	// the client addresses the wire name, which for player-race pets carries the
	// MakeNameUnique suffix, and charmed NPCs get a "_CHARM" suffix in some
	// paths.  Compare case-insensitively against both the raw and cleaned names.
	if (!targetname || !*targetname) return false;
	auto name_matches = [&](const char* candidate) -> bool {
		if (!candidate || !*candidate) return false;
		return strncasecmp(candidate, targetname, strlen(targetname)) == 0 ||
		       strncasecmp(targetname, candidate, strlen(candidate)) == 0;
	};
	if (!name_matches(mypet->GetName()) && !name_matches(mypet->GetCleanName())) {
		return false; // a genuine tell to another player
	}

	// Uppercase a trimmed copy for prefix matching.
	char upper[128] = {};
	{
		const char* p = msg;
		while (*p == ' ') ++p;
		size_t n = 0;
		for (; p[n] && n < sizeof(upper) - 1; ++n) {
			upper[n] = static_cast<char>(toupper(static_cast<unsigned char>(p[n])));
		}
		upper[n] = '\0';
	}

	auto starts_with = [&](const char* kw) -> bool {
		return strncmp(upper, kw, strlen(kw)) == 0;
	};

	// Longest phrases first so "GUARD ME" is not swallowed by "GUARD".
	// is_control marks the orders that DIRECT the pet, as opposed to the three
	// that merely query or dismiss it — see the Enchanter gate below.
	uint32 command;
	bool   is_control = true;
	if      (starts_with("ATTACK"))        command = PET_ATTACK;
	else if (starts_with("BACK OFF"))      command = PET_BACKOFF;
	else if (starts_with("AS YOU WERE"))   command = PET_BACKOFF;
	else if (starts_with("GET LOST"))    { command = PET_GETLOST;      is_control = false; }
	else if (starts_with("GUARD ME"))      command = PET_FOLLOWME;
	else if (starts_with("GUARD HERE"))    command = PET_GUARDHERE;
	else if (starts_with("GUARD"))         command = PET_GUARDHERE;
	else if (starts_with("FOLLOW ME"))     command = PET_FOLLOWME;
	else if (starts_with("FOLLOW"))        command = PET_FOLLOWME;
	else if (starts_with("REPORT HEALTH")){ command = PET_HEALTHREPORT; is_control = false; }
	else if (starts_with("HEALTH"))      { command = PET_HEALTHREPORT; is_control = false; }
	else if (starts_with("WHO LEADER"))  { command = PET_LEADER;        is_control = false; }
	else if (starts_with("LEADER"))      { command = PET_LEADER;        is_control = false; }
	// v29c also emits a bare "master" for the leader query.  EQClassic does not
	// handle this verb either, so it is mapped on semantics rather than from a
	// reference implementation — same question, same answer.
	else if (starts_with("MASTER"))      { command = PET_LEADER;        is_control = false; }
	else if (starts_with("TAUNT"))         command = PET_TAUNT;
	else if (starts_with("SIT"))           command = PET_SITDOWN;
	else if (starts_with("STAND"))         command = PET_STANDUP;
	else {
		// Not a recognised order.  Log it rather than swallowing it — an
		// unmapped /pet verb should show up here rather than vanishing into
		// the chat system as a tell to a mob.
		LogInfo("[TrilogyZone] PetCommand: char={} pet=[{}] UNRECOGNISED order=[{}]",
		        s.char_name, mypet->GetCleanName(), msg);
		return false;
	}

	// Enchanter Animation pets are reactive by design and take no orders — this
	// is classic behaviour, not a limitation.  A CHARMED pet is a different
	// thing and obeys normally, so the gate is on the pet, not just the class.
	// Mirrors EQClassic (Client_Messaging.cpp:221), including the scope: their
	// class check wraps only the directing orders, leaving get-lost / leader /
	// report-health available, so an Enchanter can still dismiss or query an
	// Animation even though it will not take direction.
	if (is_control &&
	    s.trilogy_client->GetClass() == Class::Enchanter &&
	    !mypet->IsCharmed()) {
		LogInfo("[TrilogyZone] PetCommand: char={} pet=[{}] order=[{}] refused "
		        "(Enchanter Animation pets are reactive)",
		        s.char_name, mypet->GetCleanName(), msg);
		return true; // consumed — must not fall through to the chat system
	}

	// PET_ATTACK needs a target; the client leaves it to the server's notion of
	// the player's current target, which OP_ClientTarget already keeps current.
	uint32 target_id = 0;
	if (command == PET_ATTACK || command == PET_LEADER) {
		if (Mob* t = s.trilogy_client->GetTarget()) {
			target_id = static_cast<uint32>(t->GetID());
		}
	}

	LogInfo("[TrilogyZone] PetCommand: char={} pet=[{}] order=[{}] -> command={} target={}",
	        s.char_name, mypet->GetCleanName(), msg, command, target_id);

	auto* app = new EQApplicationPacket(OP_PetCommands, sizeof(PetCommand_Struct));
	auto* pc  = reinterpret_cast<PetCommand_Struct*>(app->pBuffer);
	pc->command = command;
	pc->target  = target_id;
	s.trilogy_client->Handle_OP_PetCommands(app);
	delete app;

	return true;
}

// ============================================================
// WireOwnerIdForSession — Spawn_Struct::pet_owner_id (offset 66)
//
// EQClassic sets this unconditionally for every mob it packs
// (Zone/Source/mob.cpp:333, `ns->spawn.pet_owner_id = ownerid`).
// We never wrote the field at all, so it was always 0 — the v29c
// "not a pet" sentinel — and the client consequently refused every
// pet command with "you have no pet to control", never putting a
// packet on the wire for us to handle.
//
// The value is per-observer, not a straight copy of GetOwnerID().
// A Trilogy client knows itself by the synthetic player_spawn_id
// minted at zone entry (0x4000 | char_id, see HandleZoneEntry) and
// every other entity by its EQEmu entity id.  So a pet owned by the
// observer has to carry that observer's player_spawn_id; sending the
// raw entity id means the client's "is this mine?" comparison fails
// and the pet reads as somebody else's.
// ============================================================
int16_t TrilogyZoneServer::WireOwnerIdForSession(const Session& s, uint16_t owner_entity_id)
{
	if (owner_entity_id == 0) return 0; // not a pet

	// TranslateId() already encapsulates the self-substitution rule.
	if (s.trilogy_client) {
		return static_cast<int16_t>(
		    s.trilogy_client->TranslateId(static_cast<uint32_t>(owner_entity_id)));
	}
	return static_cast<int16_t>(owner_entity_id);
}

int16_t TrilogyZoneServer::WireOwnerIdForSessionKey(uint64_t session_key, uint16_t owner_entity_id)
{
	if (owner_entity_id == 0) return 0;

	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return static_cast<int16_t>(owner_entity_id);
	return WireOwnerIdForSession(it->second, owner_entity_id);
}

// Look up another Trilogy session in this zone by its wire entity id.  Each
// TrilogyClient registers itself in entity_list under its player_spawn_id which
// is also stored on the session, so a linear scan is fine here (handful of
// players per zone).  Returns nullptr if no Trilogy session matches.
TrilogyZoneServer::Session* TrilogyZoneServer::FindSessionByEntityId(uint16_t entity_id)
{
	for (auto& kv : m_sessions) {
		Session& s = kv.second;
		if (!s.trilogy_client) continue;
		if (static_cast<uint16_t>(s.trilogy_client->GetID()) == entity_id)
			return &s;
		if (s.player_spawn_id == entity_id)
			return &s;
	}
	return nullptr;
}

// ============================================================
// Bot ^invgive cursor materialization
//
// Bot::FinishTrade(BotTradeClientNoDropNoTrade) reads the cursor item via
// m_inv.GetItem(slotCursor).  On Trilogy, the HandleMoveItem pickup path
// (wire to_slot == 0) only stashes s.cursor_from_db — the item itself stays
// at its source DB slot and m_inv.cursor is empty — so FinishTrade silently
// no-ops and the player sees no feedback.
//
// These helpers materialize the cursor item into m_inv.cursor + DB slot 33
// before FinishTrade runs, then clean up the source DB row + worn-slot
// side-effects + visual wire cursor after.
// ============================================================
int TrilogyZoneServer::MaterializeCursorForBotTrade(TrilogyClient* tc)
{
	if (!tc) return -1;
	Session* sp = FindSessionByEntityId(static_cast<uint16_t>(tc->GetID()));
	if (!sp) return -1;
	Session& s = *sp;

	// If m_inv.cursor is already populated (e.g. #si, summon, loot), the
	// shared FinishTrade path will work as-is.  Treat as "no cleanup needed"
	// by returning the sentinel 33 (FinalizeCursorAfterBotTrade no-ops on it).
	if (tc->GetInv().GetItem(EQ::invslot::slotCursor)) {
		LogInfo("[TrilogyZone] BotTrade: cursor already in m_inv for char={} — no materialization needed",
		        s.char_id);
		return 33;
	}

	// Resolve source DB slot — first try the session-tracked pickup slot,
	// then fall back to DB slot 33 / 8000-8010 (cursor queue).
	int src_db = s.cursor_from_db;
	if (src_db < 0) {
		auto r = database.QueryDatabase(fmt::format(
		    "SELECT `slotid` FROM `inventory` WHERE `charid`={} AND "
		    "(`slotid`=33 OR (`slotid` BETWEEN 8000 AND 8010)) "
		    "ORDER BY `slotid` ASC LIMIT 1", s.char_id));
		if (r.Success() && r.RowCount() > 0) {
			src_db = static_cast<int>(Strings::ToInt(r.begin()[0]));
		}
	}
	if (src_db < 0) {
		LogInfo("[TrilogyZone] BotTrade: char={} cursor empty (no cursor_from_db, no DB row)",
		        s.char_id);
		return -1;
	}

	// Read item from DB at the source slot.
	auto r = database.QueryDatabase(fmt::format(
	    "SELECT `itemid`, `charges`, `color`, `augslot1`, `augslot2`, `augslot3`, "
	    "`augslot4`, `augslot5`, `augslot6` "
	    "FROM `inventory` WHERE `charid`={} AND `slotid`={}", s.char_id, src_db));
	if (!r.Success() || r.RowCount() == 0) {
		LogInfo("[TrilogyZone] BotTrade: char={} src_db={} has no inventory row", s.char_id, src_db);
		return -1;
	}

	auto row = r.begin();
	const uint32 item_id = static_cast<uint32>(Strings::ToInt(row[0]));
	if (item_id == 0) return -1;
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
		LogInfo("[TrilogyZone] BotTrade: char={} CreateItem({}) failed", s.char_id, item_id);
		return -1;
	}
	inst->SetColor(color);

	auto& inv = tc->GetInv();
	inv.PushCursor(*inst);
	delete inst;

	// Persist the new cursor queue (DB slot 33) and remove the source row in
	// one shot so DB and m_inv stay consistent regardless of whether the
	// subsequent FinishTrade succeeds or bails into ResetTrade.
	{
		auto sc = inv.cursor_cbegin(), ec = inv.cursor_cend();
		database.SaveCursor(s.char_id, sc, ec);
	}
	if (src_db != 33) {
		database.QueryDatabase(fmt::format(
		    "DELETE FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    s.char_id, src_db));
		// Mirror the DB delete in m_inv so FindFreeSlot/CalcBonuses/lore
		// checks don't see a ghost.  Worn slots are handled downstream by
		// Finalize's RefreshWornSlotsAfterMove (full equip-event chain).
		auto is_worn = [](int slot) {
			return (slot >= 1 && slot <= 20) || slot == EQ::invslot::slotAmmo;
		};
		if (!is_worn(src_db)) {
			auto* old = inv.PopItem(static_cast<int16>(src_db));
			safe_delete(old);
		}
	}

	LogInfo("[TrilogyZone] BotTrade: materialized cursor for char={} from DB slot {} (item_id={})",
	        s.char_id, src_db, item_id);
	return src_db;
}

void TrilogyZoneServer::FinalizeCursorAfterBotTrade(TrilogyClient* tc, int src_db)
{
	if (!tc) return;
	Session* sp = FindSessionByEntityId(static_cast<uint16_t>(tc->GetID()));
	if (!sp) return;
	Session& s = *sp;

	auto& inv = tc->GetInv();

	// Cursor tracking is consumed — the source slot has either been transferred
	// to the bot or returned via the cursor queue.  Either way the next
	// HandleMoveItem(0 → ...) must NOT reference the old src.
	s.cursor_from_db = -1;

	// Detect FinishTrade rejection.  On success it calls DeleteItemInInventory
	// (slotCursor) which pops m_inv.cursor.  On rejection (class/race/level,
	// no slot fit, etc.) it leaves the item on cursor with no movement action.
	// src_db==33 is the sentinel "cursor was already populated; Materialize
	// no-oped" — no rollback needed for that case.
	const bool trade_rejected =
	    (src_db != 33) && (inv.GetItem(EQ::invslot::slotCursor) != nullptr);

	if (trade_rejected) {
		// Materialize already DELETEd DB[src_db] and (for non-worn) popped
		// m_inv[src_db].  Put the cursor item back where it came from so the
		// player's visible state matches "nothing happened."
		EQ::ItemInstance* restore = inv.GetItem(EQ::invslot::slotCursor)->Clone();

		// Drain the cursor queue and persist empty so DB[33] / DB[8000-8999]
		// are cleared.
		while (auto* p = inv.PopItem(EQ::invslot::slotCursor)) {
			safe_delete(p);
		}
		{
			auto sc = inv.cursor_cbegin(), ec = inv.cursor_cend();
			database.SaveCursor(s.char_id, sc, ec);
		}

		// Restore DB row + m_inv at src_db.  PutItem internally DeleteItem's
		// any existing entry first, so this is safe whether the slot is
		// currently empty (non-worn) or still holds the original (worn,
		// skipped by Materialize).
		database.SaveInventory(s.char_id, restore, static_cast<int16>(src_db));
		inv.PutItem(static_cast<int16>(src_db), *restore);

		// Re-deliver to the client so the visual appears in src_db.  Trilogy
		// translates ItemPacketTrade → 0x3120 with the right wire slot.
		tc->SendItemPacket(static_cast<int16>(src_db), restore, ItemPacketTrade);

		// Clear the visual cursor on v29c (no OP_DeleteItem in this client).
		Trilogy::structs::MoveItem_Struct mv{};
		mv.from_slot       = 0;
		mv.to_slot         = 0xFFFFFFFFu;
		mv.number_in_stack = 0;
		SendApp(s.source_addr, s.source_port, s, ZN_OP_MoveItem,
		        reinterpret_cast<const uint8_t*>(&mv), sizeof(mv));

		LogInfo("[TrilogyZone] BotTrade: trade rejected — restored item to DB slot {} for char={}",
		        src_db, s.char_id);

		safe_delete(restore);
		tc->Save();
		return;
	}

	// Trade accepted.  Run the standard cleanup.
	//
	// If the source was a worn slot (1-20 / ammo 22) the player's m_inv at that
	// slot is now stale (Materialize DELETEd the DB row without firing equip
	// events).  Reuse the same helper HandleMoveItem uses: it re-reads the
	// slot from DB (now empty), pops m_inv, fires EVENT_UNEQUIP_ITEM, recalcs
	// bonuses, refreshes ApplyWeaponsStance + SetAttackTimer.
	// Sentinel src_db == 33 means cursor was already DB-synced before
	// Materialize ran — no source-slot row was touched, so skip the refresh.
	if (src_db >= 1 && (src_db <= 20 || src_db == EQ::invslot::slotAmmo)) {
		RefreshWornSlotsAfterMove(s, src_db, -1, /*destroy_path=*/true);
	}

	// Visual cursor sync: m_inv.cursor is empty (FinishTrade transferred the
	// item to the bot) but the wire-level cursor still shows the original
	// item.  v29c has no OP_DeleteItem; the EQClassic pattern (see
	// HandleMemorizeSpell) is OP_MoveItem(from=0, to=0xFFFFFFFF).
	if (!inv.GetItem(EQ::invslot::slotCursor)) {
		Trilogy::structs::MoveItem_Struct mv{};
		mv.from_slot       = 0;
		mv.to_slot         = 0xFFFFFFFFu;
		mv.number_in_stack = 0;
		SendApp(s.source_addr, s.source_port, s, ZN_OP_MoveItem,
		        reinterpret_cast<const uint8_t*>(&mv), sizeof(mv));
		LogInfo("[TrilogyZone] BotTrade: cleared wire cursor for char={}", s.char_id);
	}

	tc->Save();
}

// Wire ↔ DB slot mapping for the trade handlers (mirrors HandleMoveItem;
// shared by NPC + PC paths).  Wire slot 0 = cursor (DB 33 by default, or the
// session's tracked cursor_from_db after a two-step unequip pickup).
int TrilogyZoneServer::TradeWireToDb(const Session& s, uint32_t w)
{
	if (w == 0)               return (s.cursor_from_db >= 0) ? s.cursor_from_db : 33;
	if (w >= 1  && w <= 20)   return static_cast<int>(w);
	if (w >= 21 && w <= 29)   return static_cast<int>(w) + 1;
	if (w >= 250 && w <= 339) return static_cast<int>(w) + 1;
	return -1;
}

// DB content-slot base for the 10 contents of a bag at the given top slot,
// or -1 if that slot can't hold a container.  Matches the inline lambda in
// HandleMoveItem (general 23-30 → 251 + (slot-23)*10).
int TrilogyZoneServer::TradeContBaseFor(int db_slot)
{
	if (db_slot >= 23   && db_slot <= 30)   return 251  + (db_slot - 23)   * 10;
	if (db_slot >= 2000 && db_slot <= 2007) return 2031 + (db_slot - 2000) * 10;
	return -1;
}

// Defined non-static in trilogy_client.cpp; reused here to build the same
// 292-byte ClassicItem_Struct for OP_TradeItemPacket (0xdf20) notifications.
extern bool BuildClassicItemFromInst(const EQ::ItemInstance* inst,
                                     Trilogy::structs::ClassicItem_Struct& ci,
                                     int16_t equip_slot);

// Deliver a freshly-traded item into a recipient TrilogyClient's inventory
// display mid-session.  Uses ItemPacketTrade so HandleItemPacket routes to
// OP_ItemTradeIn (0x3120) for items / bag contents and OP_CPlayerCont (0x6621)
// for top-level bag delivery (mirrors merchant buy + loot delivery paths).
static void DeliverTradedItemToTrilogyClient(TrilogyClient* recipient,
                                             int16_t        dest_db_slot,
                                             uint32_t       item_id,
                                             int16_t        charges)
{
	if (!recipient) return;
	EQ::ItemInstance* inst = database.CreateItem(item_id, charges);
	if (!inst) return;
	recipient->SendItemPacket(dest_db_slot, inst, ItemPacketTrade);
	safe_delete(inst);
}

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

	if (other_id == self_id) {
		// Can't trade with yourself — ignore.
		return;
	}

	Mob* other = entity_list.GetMob(other_id);
	if (!other) return;

	// ── NPC trade ────────────────────────────────────────────────────────────
	if (other->IsNPC()) {
		// Clear any leftover staged state from a previous (aborted) trade.
		// v29c doesn't always send OP_CancelTrade on window-close, so items
		// staged from cursor last time may still be orphaned in the DB —
		// clean those up before we drop the metadata (same reason
		// HandleTradeCancel does it).  Partial-pickup cursors also get their
		// refund here so we don't strand items across a spam-click reopen.
		RefundPartialCursorTradeItems(s);
		CleanupOrphanedCursorTradeItems(s);
		for (auto& st : s.trade_items) st = Session::TradeStageItem{};
		s.trade_cp = s.trade_sp = s.trade_gp = s.trade_pp = 0;
		s.trade_npc_id = other_id;

		// Echo OP_TradeAccepted with the ids swapped so the client opens its window.
		uint8_t resp[8] = {};
		*reinterpret_cast<uint32_t*>(resp)     = id_b;
		*reinterpret_cast<uint32_t*>(resp + 4) = id_a;
		SendApp(addr, port, s, ZN_OP_TradeAccept, resp, 8);

		LogInfo("[TrilogyZone] Trade opened: {} <-> NPC entity {}", s.char_name, other_id);
		return;
	}

	// ── PC-to-PC trade (Trilogy↔Trilogy only) ────────────────────────────────
	if (!other->IsClient() || !other->CastToClient()->IsTrilogyClient()) {
		// Cross-version PC trade is out of scope; ignore quietly.
		return;
	}

	// Range gate (same threshold as NPC trade — interaction range).
	if (DistanceSquared(s.trilogy_client->GetPosition(), other->GetPosition()) > USE_NPC_RANGE2) {
		s.trilogy_client->Message(Chat::Red, "You are too far away to trade.");
		return;
	}

	Session* partner = FindSessionByEntityId(other_id);
	if (!partner || !partner->trilogy_client) {
		LogInfo("[TrilogyZone] PCTrade request: no Trilogy session for entity {}", other_id);
		return;
	}

	// Both sides busy with anything else?  Reject.
	if (s.trade_npc_id || partner->trade_npc_id ||
	    s.pc_trade_active || partner->pc_trade_active) {
		s.trilogy_client->Message(Chat::Red, "You or your target are already in a trade.");
		return;
	}

	// Initialise PC-trade state on BOTH sessions.  We set up the recipient too so
	// their incoming stage/coin packets find pc_trade_active=true on the relay.
	auto init_pc_trade = [](Session& sess, uint16_t partner_entity, uint32_t partner_char) {
		sess.pc_trade_active     = true;
		sess.pc_trade_partner_id = partner_entity;
		sess.pc_trade_partner_ch = partner_char;
		sess.pc_trade_gave       = false;
		for (auto& it : sess.pc_trade_main) it = Session::PcTradeStageItem{};
		for (auto& row : sess.pc_trade_bag) for (auto& it : row) it = Session::PcTradeBagSlot{};
		sess.pc_trade_offer_cp = sess.pc_trade_offer_sp = 0;
		sess.pc_trade_offer_gp = sess.pc_trade_offer_pp = 0;
	};
	init_pc_trade(s,        other_id, partner->char_id);
	init_pc_trade(*partner, self_id,  s.char_id);

	// Open the trade window on BOTH sides by sending OP_TradeAccepted (0xe620).
	// v29c only renders the trade window in response to 0xe620 — forwarding the
	// raw 0xd120 packet is silently discarded by the receiving client (proven by
	// the 2026-06-21 log where the relayed 0xd120 produced no window).  EQClassic
	// goes through a 0xd120 echo + recipient-generated 0xe620 ack, but v29c does
	// not auto-generate the ack, so we open both windows server-side immediately.
	// Convention from the NPC path: each side receives {fromid=self, toid=other}.
	auto send_open = [&](Session& sess, uint16_t self_e, uint16_t other_e) {
		uint8_t resp[8] = {};
		const uint32_t f = self_e;
		const uint32_t t = other_e;
		std::memcpy(resp + 0, &f, 4);
		std::memcpy(resp + 4, &t, 4);
		SendApp(sess.source_addr, sess.source_port, sess,
		        ZN_OP_TradeAccept, resp, 8);
	};
	send_open(s,        self_id,  other_id);
	send_open(*partner, other_id, self_id);

	LogInfo("[TrilogyZone] PCTrade opened: {} (entity {}) <-> {} (entity {})",
	        s.char_name, self_id, partner->char_name, other_id);
}

void TrilogyZoneServer::HandleTradeAccepted(const std::string& /*addr*/, int /*port*/, Session& s,
                                            const uint8_t* /*payload*/, uint32_t /*plen*/)
{
	// Inbound 0xe620 is the ACCEPT click, and it must be relayed to the other
	// party — that relay is what turns their copy of your name green.
	//
	// The earlier reading, that this packet was a redundant echo because
	// HandleTradeRequest already opened both windows, was wrong.  Opening the
	// window and accepting the trade share an opcode but are different events,
	// and only the first was being handled.  EQClassic's ProcessOP_TradeAccepted
	// (client_process.cpp:3459) does the relay in one line:
	//     tmp->CastToClient()->QueuePacket(pApp);
	// forwarding the client's own packet to the partner it looked up from
	// msg->fromid, alongside the InTrade/TradeWithEnt bookkeeping we already do
	// in HandleTradeRequest.
	//
	// It looked order-dependent because the accepter's own window is already
	// open and its client renders its own click locally; only the partner
	// depended on a packet that never arrived.  Whichever side clicked second
	// appeared to work, because by then the observer had already seen the
	// window-opening 0xe620 for that trade and had a green name to show.
	if (!s.trilogy_client) return;

	if (!s.pc_trade_active || s.pc_trade_partner_id == 0) {
		// Accept outside an active PC trade — an NPC trade or a stale click.
		// Nothing to relay; NPC trades have no second window to update.
		return;
	}

	Session* partner = FindSessionByEntityId(s.pc_trade_partner_id);
	if (!partner || !partner->trilogy_client) return;

	// Rebuild rather than forward the client's bytes.  The ids in this 8-byte
	// payload are read from the RECIPIENT's point of view — HandleTradeRequest
	// opens each window with {fromid = that side's own entity, toid = the other}
	// — so the accepter's copy cannot be passed through unchanged.
	uint8_t resp[8] = {};
	const uint32_t f = static_cast<uint32_t>(partner->trilogy_client->GetID());
	const uint32_t t = static_cast<uint32_t>(s.trilogy_client->GetID());
	std::memcpy(resp + 0, &f, 4);
	std::memcpy(resp + 4, &t, 4);

	SendApp(partner->source_addr, partner->source_port, *partner,
	        ZN_OP_TradeAccept, resp, 8);

	LogInfo("[TrilogyZone] PCTrade accept relayed | from=[{}] to=[{}]",
	        s.char_name, partner->char_name);
}

void TrilogyZoneServer::HandleTradeMoveItem(Session& s, uint32_t from_wire, uint32_t to_wire,
                                            uint32_t number_in_stack)
{
	if (!s.trilogy_client) return;

	// ── Item moved INTO a trade slot ─────────────────────────────────────────
	if (to_wire >= 3000 && to_wire <= 3007) {
		const int idx     = static_cast<int>(to_wire - 3000);
		const int from_db = TradeWireToDb(s, from_wire);
		if (from_db < 0) return;

		uint32_t item_id    = 0;
		int16_t  db_charges = 0;
		auto r = database.QueryDatabase(fmt::format(
		    "SELECT itemid, charges FROM inventory WHERE charid={} AND slotid={}",
		    s.char_id, from_db));
		if (r.Success() && r.RowCount() > 0) {
			auto row = r.begin();
			item_id    = static_cast<uint32_t>(Strings::ToInt(row[0]));
			db_charges = static_cast<int16_t>(Strings::ToInt(row[1]));
		}
		if (item_id == 0) return;

		// Partial-stack staging is now correct: HandleMoveItem materialises a
		// real cursor row at DB slot 33 (or 8000-8010 queue) on partial pickup
		// and decrements the source row.  TradeWireToDb(0) returns
		// cursor_from_db, which now points at that cursor row, so db_charges
		// already reflects the partial qty the client put on cursor.  On Give,
		// HandleTradeGive deletes the cursor row only — the source remainder
		// stays put.  This log stays as the staging trace.
		LogInfo("[TrilogyZone] TradeStageDIAG char={} item={} trade_slot={} from_db={} "
		        "db_charges={} client_number_in_stack={} from_wire={} (cursor_from_db_was={})",
		        s.char_id, item_id, idx, from_db,
		        (int)db_charges, number_in_stack, from_wire,
		        s.cursor_from_db);

		int16_t charges = db_charges;

		// ── PC trade staging ────────────────────────────────────────────────
		if (s.pc_trade_active) {
			const EQ::ItemData* item = database.GetItem(item_id);
			if (!item) return;

			// NoDrop enforcement: NoDrop items must never enter a PC trade window.
			// EQ::ItemData::NoDrop == 0 means "no drop" (same convention as
			// FinishTrade's `inst->IsDroppable()`/`item->NoDrop` checks).
			if (item->NoDrop == 0) {
				s.trilogy_client->Message(Chat::Red,
				    "You cannot trade that item to another player.");
				LogInfo("[TrilogyZone] PCTrade NoDrop reject char={} item={}",
				        s.char_id, item_id);
				return;
			}

			Session* partner = FindSessionByEntityId(s.pc_trade_partner_id);
			if (!partner || !partner->trilogy_client) {
				// Partner vanished — silently abort our side; next interaction
				// will detect and close.
				return;
			}

			s.pc_trade_main[idx].item_id                 = item_id;
			s.pc_trade_main[idx].charges                 = charges;
			s.pc_trade_main[idx].from_db_slot            = from_db;
			s.pc_trade_main[idx].original_source_db_slot = (from_wire == 0)
			    ? s.cursor_partial_origin_db
			    : -1;
			for (auto& bs : s.pc_trade_bag[idx]) bs = Session::PcTradeBagSlot{};
			if (from_wire == 0) {
				s.cursor_from_db           = -1;
				s.cursor_partial_origin_db = -1;
			}

			// Notify partner: main item at trade slot idx.
			//
			// OP_ItemToTrade wire layout (EQClassic ItemToTrade_Struct, 302 bytes
			// with the 292-byte ClassicItem_Struct embedded):
			//   /*000*/ uint32 playerid;   // RECIPIENT's entity id (the partner)
			//   /*004*/ int16  to_slot;    // trade-window slot id (0-7 main, 30+10i+j contents)
			//   /*006*/ int8   unknown;    // 0
			//   /*007*/ ClassicItem_Struct item;  // 292 B
			//   /*299*/ int8   pad[3];     // 3 B
			//
			// NB: the send_bank_trade lambda elsewhere in this file uses a
			// 2+2+1+292+5 layout; that path always sends slotid=0 because the
			// bank position is encoded inside ClassicItem_Struct.equipslot, so
			// the misalignment of slotid never bit it.  PC trade absolutely
			// relies on the outer slotid landing in v29c's to_slot field, so
			// we use the EQClassic layout faithfully.
			const uint32_t recipient_id =
			    static_cast<uint32_t>(partner->trilogy_client->GetID());

			auto send_notify = [&](uint16_t slotid, uint32_t iid, int16_t ch) {
				EQ::ItemInstance* inst = database.CreateItem(iid, ch);
				if (!inst) return;

				Trilogy::structs::ClassicItem_Struct ci{};
				const bool ok = BuildClassicItemFromInst(
				    inst, ci, static_cast<int16_t>(slotid));
				safe_delete(inst);
				if (!ok) return;

				uint8_t buf[4 + 2 + 1 + sizeof(Trilogy::structs::ClassicItem_Struct) + 3];
				std::memset(buf, 0, sizeof(buf));
				std::memcpy(buf + 0, &recipient_id, 4);
				const int16_t to_slot = static_cast<int16_t>(slotid);
				std::memcpy(buf + 4, &to_slot, 2);
				// buf[6] = 0 (unknown)
				std::memcpy(buf + 7, &ci, sizeof(ci));
				// buf[299..301] = 0 (padding)
				SendApp(partner->source_addr, partner->source_port, *partner,
				        ZN_OP_TradeItemPacket, buf, static_cast<uint32_t>(sizeof(buf)));
			};

			send_notify(static_cast<uint16_t>(idx), item_id, charges);

			// Bag-content staging: if the staged item is a bag, also stage its
			// contents and notify the partner.  Sub-slot wire convention from
			// EQClassic ProcessOP_MoveItem: trade slot i bag content j →
			// notify slot 30 + 10*i + j.
			if (item->ItemClass == EQ::item::ItemClassBag) {
				const int content_base = TradeContBaseFor(from_db);
				if (content_base >= 0) {
					auto cr = database.QueryDatabase(fmt::format(
					    "SELECT slotid, itemid, charges FROM inventory "
					    "WHERE charid={} AND slotid BETWEEN {} AND {}",
					    s.char_id, content_base, content_base + 9));
					if (cr.Success()) {
						for (auto row = cr.begin(); row != cr.end(); ++row) {
							const int sub_db   = Strings::ToInt(row[0]);
							const uint32_t iid = static_cast<uint32_t>(Strings::ToInt(row[1]));
							const int16_t  ch  = static_cast<int16_t>(Strings::ToInt(row[2]));
							const int j = sub_db - content_base;
							if (j < 0 || j > 9 || iid == 0) continue;
							s.pc_trade_bag[idx][j].item_id      = iid;
							s.pc_trade_bag[idx][j].charges      = ch;
							s.pc_trade_bag[idx][j].from_db_slot = sub_db;
							send_notify(static_cast<uint16_t>(30 + 10 * idx + j), iid, ch);
						}
					}
				}
			}

			LogInfo("[TrilogyZone] PCTrade stage char={} -> partner_char={} "
			        "item={} slot_idx={} (db_slot={})",
			        s.char_id, s.pc_trade_partner_ch, item_id, idx, from_db);
			return;
		}

		// ── NPC trade staging (existing behaviour) ──────────────────────────
		s.trade_items[idx].item_id                 = item_id;
		s.trade_items[idx].charges                 = charges;
		s.trade_items[idx].from_db_slot            = from_db;
		// Carry forward the partial-pickup origin so cancel / non-quest give
		// can merge the cursor row back into the source slot.
		s.trade_items[idx].original_source_db_slot = (from_wire == 0)
		    ? s.cursor_partial_origin_db
		    : -1;
		if (from_wire == 0) {
			s.cursor_from_db           = -1;
			s.cursor_partial_origin_db = -1;
		}

		LogInfo("[TrilogyZone] Trade stage char={} item={} -> slot {} (db_slot={} orig_src={})",
		        s.char_id, item_id, idx, from_db,
		        s.trade_items[idx].original_source_db_slot);
		return;
	}

	// ── Item moved OUT of a trade slot ──────────────────────────────────────
	if (from_wire >= 3000 && from_wire <= 3007) {
		const int idx = static_cast<int>(from_wire - 3000);
		if (s.pc_trade_active) {
			s.pc_trade_main[idx] = Session::PcTradeStageItem{};
			for (auto& bs : s.pc_trade_bag[idx]) bs = Session::PcTradeBagSlot{};
		}
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
	// int32 amount (8).  coin_type: 0=copper 1=silver 2=gold 3=platinum.
	const uint8_t  coin_type = payload[4];
	const uint32_t amount    = *reinterpret_cast<const uint32_t*>(payload + 8);
	if (amount == 0) return;

	// ── PC trade: accumulate offer + notify partner ──────────────────────────
	if (s.pc_trade_active) {
		switch (coin_type) {
			case 0: s.pc_trade_offer_cp += amount; break;
			case 1: s.pc_trade_offer_sp += amount; break;
			case 2: s.pc_trade_offer_gp += amount; break;
			case 3: s.pc_trade_offer_pp += amount; break;
			default: return;
		}

		Session* partner = FindSessionByEntityId(s.pc_trade_partner_id);
		if (partner && partner->trilogy_client) {
			// Forward TradeCoin_Struct to partner with trader = partner's entity id
			// (matches EQClassic: server stamps the OTHER player's id and the client
			// uses it to identify which side of the trade window to update).
			uint8_t out[22];
			std::memset(out, 0, sizeof(out));
			const uint32_t partner_id =
			    static_cast<uint32_t>(partner->trilogy_client->GetID());
			std::memcpy(out + 0, &partner_id, 4);
			out[4] = coin_type;
			const uint16_t magic = 0x4fD2; // EQClassic sentinel
			std::memcpy(out + 5, &magic, 2);
			std::memcpy(out + 8, &amount, 4);
			SendApp(partner->source_addr, partner->source_port, *partner,
			        ZN_OP_TradeCoins, out, static_cast<uint32_t>(sizeof(out)));
		}

		LogInfo("[TrilogyZone] PCTrade coins char={} type={} amount={}",
		        s.char_name, coin_type, amount);
		return;
	}

	// ── NPC trade: existing accumulation ────────────────────────────────────
	switch (coin_type) {
		case 0: s.trade_cp += amount; break;
		case 1: s.trade_sp += amount; break;
		case 2: s.trade_gp += amount; break;
		case 3: s.trade_pp += amount; break;
		default: return;
	}

	LogInfo("[TrilogyZone] Trade coins char={} type={} amount={}",
	        s.char_name, coin_type, amount);
}

// Refund THIS session's offered coins to its PlayerProfile carried money and
// fire OP_TradeMoneyUpdate via AddMoneyToPP.  Used by abort paths (cancel /
// disconnect / failed commit precheck).
void TrilogyZoneServer::PcTradeRefundOfferedCoins(Session& s)
{
	if (!s.trilogy_client) return;
	const uint32_t cp = s.pc_trade_offer_cp;
	const uint32_t sp = s.pc_trade_offer_sp;
	const uint32_t gp = s.pc_trade_offer_gp;
	const uint32_t pp = s.pc_trade_offer_pp;
	if (cp || sp || gp || pp)
		s.trilogy_client->AddMoneyToPP(cp, sp, gp, pp, true);
	s.pc_trade_offer_cp = s.pc_trade_offer_sp = 0;
	s.pc_trade_offer_gp = s.pc_trade_offer_pp = 0;
}

void TrilogyZoneServer::PcTradeClearState(Session& s)
{
	s.pc_trade_active     = false;
	s.pc_trade_partner_id = 0;
	s.pc_trade_partner_ch = 0;
	s.pc_trade_gave       = false;
	for (auto& it : s.pc_trade_main) it = Session::PcTradeStageItem{};
	for (auto& row : s.pc_trade_bag)
		for (auto& it : row) it = Session::PcTradeBagSlot{};
	s.pc_trade_offer_cp = s.pc_trade_offer_sp = 0;
	s.pc_trade_offer_gp = s.pc_trade_offer_pp = 0;
}

void TrilogyZoneServer::PcTradeAbortBoth(Session& s, Session* partner,
                                          const char* my_msg, const char* partner_msg)
{
	uint8_t z = 0;
	if (my_msg && *my_msg && s.trilogy_client)
		s.trilogy_client->Message(Chat::Red, my_msg);
	PcTradeRefundOfferedCoins(s);
	// Refund any partial-pickup cursor rows BEFORE PcTradeClearState wipes
	// the pc_trade_main array (the refund needs from_db_slot + origin).
	RefundPartialCursorPcTradeItems(s);
	SendApp(s.source_addr, s.source_port, s, ZN_OP_CloseTrade, &z, 0);
	PcTradeClearState(s);

	if (partner && partner->trilogy_client) {
		if (partner_msg && *partner_msg)
			partner->trilogy_client->Message(Chat::Red, partner_msg);
		PcTradeRefundOfferedCoins(*partner);
		RefundPartialCursorPcTradeItems(*partner);
		SendApp(partner->source_addr, partner->source_port, *partner,
		        ZN_OP_CloseTrade, &z, 0);
		PcTradeClearState(*partner);
	}
}

void TrilogyZoneServer::HandleTradeGive(const std::string& addr, int port, Session& s)
{
	if (!s.trilogy_client) return;

	// ── PC-to-PC commit path ────────────────────────────────────────────────
	if (s.pc_trade_active) {
		s.pc_trade_gave = true;

		Session* partner = FindSessionByEntityId(s.pc_trade_partner_id);
		if (!partner || !partner->trilogy_client) {
			// Partner gone — treat as cancel for us.  Items unchanged in DB.
			s.trilogy_client->Message(Chat::Red, "Your trade partner is no longer here.");
			PcTradeRefundOfferedCoins(s);
			uint8_t z = 0;
			SendApp(addr, port, s, ZN_OP_CloseTrade, &z, 0);
			PcTradeClearState(s);
			return;
		}

		// Wait for the partner's Give.  Relay our 0xda20 so their window can
		// show "waiting for accept" if v29c renders that state.
		if (!partner->pc_trade_gave) {
			uint8_t empty = 0;
			SendApp(partner->source_addr, partner->source_port, *partner,
			        ZN_OP_ClickGive, &empty, 0);
			LogInfo("[TrilogyZone] PCTrade give (waiting): {} → {}",
			        s.char_name, partner->char_name);
			return;
		}

		// Both Gave — run the precheck.
		auto count_free_slots = [](uint32 char_id) -> std::vector<int> {
			// Free general slots 23-30 plus free bag-content slots inside any
			// equipped containers.  Sorted in delivery order.
			std::vector<int> free_slots;
			bool occ[331] = {};
			int  bagslots[31] = {};
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
			for (int sl = 23; sl <= 30; ++sl) if (!occ[sl]) free_slots.push_back(sl);
			for (int G = 23; G <= 30; ++G) {
				if (bagslots[G] <= 0) continue;
				const int base = 251 + (G - 23) * 10;
				const int n    = bagslots[G] > 10 ? 10 : bagslots[G];
				for (int j = 0; j < n; ++j)
					if (base + j <= 330 && !occ[base + j]) free_slots.push_back(base + j);
			}
			return free_slots;
		};

		// Count items each side will receive (skip stale / disappeared slots).
		auto reconcile = [&](Session& src) -> std::vector<int> {
			// Returns indices [0..7] of pc_trade_main slots whose DB row still
			// matches the staged item_id (mid-trade reshuffle guard).
			std::vector<int> live;
			for (int i = 0; i < 8; ++i) {
				const auto& st = src.pc_trade_main[i];
				if (st.item_id == 0) continue;
				auto r = database.QueryDatabase(fmt::format(
				    "SELECT 1 FROM inventory WHERE charid={} AND slotid={} AND itemid={}",
				    src.char_id, st.from_db_slot, st.item_id));
				if (r.Success() && r.RowCount() > 0) live.push_back(i);
			}
			return live;
		};

		std::vector<int> a_to_b = reconcile(s);
		std::vector<int> b_to_a = reconcile(*partner);

		// Lore precheck (both ways) using DB-authoritative override.
		auto lore_blocked = [](TrilogyClient* receiver, const Session& src,
		                        const std::vector<int>& live) -> uint32_t {
			for (int i : live) {
				const EQ::ItemData* it = database.GetItem(src.pc_trade_main[i].item_id);
				if (!it) continue;
				if (receiver->CheckLoreConflict(it)) return it->ID;
				// Lore inside bag contents too.
				if (it->ItemClass == EQ::item::ItemClassBag) {
					for (const auto& bs : src.pc_trade_bag[i]) {
						if (bs.item_id == 0) continue;
						const EQ::ItemData* sub = database.GetItem(bs.item_id);
						if (sub && receiver->CheckLoreConflict(sub)) return sub->ID;
					}
				}
			}
			return 0;
		};

		if (uint32_t bad = lore_blocked(partner->trilogy_client, s, a_to_b)) {
			LogInfo("[TrilogyZone] PCTrade abort: lore conflict on partner side (item {})", bad);
			PcTradeAbortBoth(s, partner,
			    "Your trade partner cannot accept that Lore item.",
			    "You already own a Lore item being offered.");
			return;
		}
		if (uint32_t bad = lore_blocked(s.trilogy_client, *partner, b_to_a)) {
			LogInfo("[TrilogyZone] PCTrade abort: lore conflict on our side (item {})", bad);
			PcTradeAbortBoth(s, partner,
			    "You already own a Lore item being offered.",
			    "Your trade partner cannot accept that Lore item.");
			return;
		}

		// Free-slot precheck.  Each main item needs one top-level destination
		// slot on the receiver; bag contents follow inside the bag (no top-slot
		// allocation required).
		//
		// A side's outgoing items will VACATE their source slots when executed
		// before the inbound delivery runs, so the receiver's "effective" free
		// slot count = current_free + their_own_outgoing_count.  Without this
		// adjustment a 1-for-1 swap with a full inventory would falsely fail.
		std::vector<int> s_free       = count_free_slots(s.char_id);
		std::vector<int> partner_free = count_free_slots(partner->char_id);

		const size_t partner_effective = partner_free.size() + b_to_a.size();
		const size_t s_effective       = s_free.size()       + a_to_b.size();

		if (a_to_b.size() > partner_effective) {
			LogInfo("[TrilogyZone] PCTrade abort: partner has no space ({} needed, {} effective free)",
			        a_to_b.size(), partner_effective);
			PcTradeAbortBoth(s, partner,
			    "Your trade partner does not have enough inventory space.",
			    "You do not have enough inventory space.");
			return;
		}
		if (b_to_a.size() > s_effective) {
			LogInfo("[TrilogyZone] PCTrade abort: we have no space ({} needed, {} effective free)",
			        b_to_a.size(), s_effective);
			PcTradeAbortBoth(s, partner,
			    "You do not have enough inventory space.",
			    "Your trade partner does not have enough inventory space.");
			return;
		}

		// ── Execute (atomic-ish — DELETE+INSERT per item) ──────────────────
		// dst_free is re-queried at the start of each direction so that slots
		// freed by the previous direction's execute (receiver's outgoing items
		// vacating) are visible.  A bag MUST land in a general-inventory slot
		// (23-30); other items can use any free slot the count_free_slots
		// helper returned.
		auto execute_side = [&](Session& src, Session& dst,
		                         const std::vector<int>& live) -> bool {
			std::vector<int> dst_free = count_free_slots(dst.char_id);
			for (int i : live) {
				const auto& st = src.pc_trade_main[i];
				const EQ::ItemData* item = database.GetItem(st.item_id);
				if (!item) continue;

				int chosen_idx = -1;
				if (item->ItemClass == EQ::item::ItemClassBag) {
					for (size_t k = 0; k < dst_free.size(); ++k) {
						if (dst_free[k] >= 23 && dst_free[k] <= 30) {
							chosen_idx = static_cast<int>(k);
							break;
						}
					}
				} else if (!dst_free.empty()) {
					chosen_idx = 0;
				}
				if (chosen_idx < 0) {
					LogInfo("[TrilogyZone] PCTrade execute: no destination for item {} (bag={}); skipping",
					        st.item_id,
					        (item->ItemClass == EQ::item::ItemClassBag) ? 1 : 0);
					continue;
				}
				const int dst_slot = dst_free[chosen_idx];
				dst_free.erase(dst_free.begin() + chosen_idx);

				database.QueryDatabase(fmt::format(
				    "DELETE FROM inventory WHERE charid={} AND slotid={} AND itemid={}",
				    src.char_id, st.from_db_slot, st.item_id));

				database.QueryDatabase(fmt::format(
				    "INSERT INTO inventory (charid, slotid, itemid, charges) "
				    "VALUES ({}, {}, {}, {})",
				    dst.char_id, dst_slot, st.item_id, (int)st.charges));

				DeliverTradedItemToTrilogyClient(dst.trilogy_client,
				    static_cast<int16_t>(dst_slot), st.item_id, st.charges);

				if (item->ItemClass == EQ::item::ItemClassBag) {
					const int src_base = TradeContBaseFor(st.from_db_slot);
					const int dst_base = TradeContBaseFor(dst_slot);
					if (src_base >= 0 && dst_base >= 0) {
						for (int j = 0; j < 10; ++j) {
							const auto& bs = src.pc_trade_bag[i][j];
							if (bs.item_id == 0) continue;
							database.QueryDatabase(fmt::format(
							    "DELETE FROM inventory WHERE charid={} AND slotid={} AND itemid={}",
							    src.char_id, bs.from_db_slot, bs.item_id));
							database.QueryDatabase(fmt::format(
							    "INSERT INTO inventory (charid, slotid, itemid, charges) "
							    "VALUES ({}, {}, {}, {})",
							    dst.char_id, dst_base + j, bs.item_id, (int)bs.charges));
							DeliverTradedItemToTrilogyClient(dst.trilogy_client,
							    static_cast<int16_t>(dst_base + j), bs.item_id, bs.charges);
						}
					}
				}

				LogInfo("[TrilogyZone] PCTrade commit: char {} -> char {} item {} db {} -> {} (bag={})",
				        src.char_id, dst.char_id, st.item_id, st.from_db_slot, dst_slot,
				        (item->ItemClass == EQ::item::ItemClassBag) ? 1 : 0);
			}
			return true;
		};

		execute_side(s,        *partner, a_to_b);
		execute_side(*partner, s,        b_to_a);

		// Coin exchange: each side receives the OTHER side's offered coins.
		// PP was already debited at stage time by HandleMoveCoin (carried→trade).
		auto pay = [](Session& dst, Session& src) {
			const uint32_t cp = src.pc_trade_offer_cp;
			const uint32_t sp = src.pc_trade_offer_sp;
			const uint32_t gp = src.pc_trade_offer_gp;
			const uint32_t pp = src.pc_trade_offer_pp;
			if (cp || sp || gp || pp)
				dst.trilogy_client->AddMoneyToPP(cp, sp, gp, pp, true);
		};
		pay(s,        *partner);
		pay(*partner, s);

		// Persist both sides.
		s.trilogy_client->Save();
		partner->trilogy_client->Save();

		// Close windows on both.
		uint8_t z = 0;
		SendApp(addr, port, s, ZN_OP_CloseTrade, &z, 0);
		SendApp(partner->source_addr, partner->source_port, *partner,
		        ZN_OP_CloseTrade, &z, 0);

		LogInfo("[TrilogyZone] PCTrade complete: {} ↔ {} (a_to_b={} b_to_a={})",
		        s.char_name, partner->char_name, a_to_b.size(), b_to_a.size());

		PcTradeClearState(s);
		PcTradeClearState(*partner);
		return;
	}

	// ── NPC give path (unchanged) ───────────────────────────────────────────
	NPC* npc = nullptr;
	if (s.trade_npc_id) {
		Mob* m = entity_list.GetMob(s.trade_npc_id);
		if (m && m->IsNPC()) npc = m->CastToNPC();
	}

	const bool quest_npc = npc && parse->HasQuestSub(npc->GetNPCTypeID(), EVENT_TRADE);

	if (quest_npc) {
		std::vector<EQ::ItemInstance*> taken(4, nullptr);
		for (int i = 0; i < 4; ++i) {
			auto& st = s.trade_items[i];
			if (st.item_id == 0) continue;
			auto r = database.QueryDatabase(fmt::format(
			    "SELECT charges FROM inventory WHERE charid={} AND slotid={} AND itemid={}",
			    s.char_id, st.from_db_slot, st.item_id));
			if (!r.Success() || r.RowCount() == 0) continue;

			// DIAG: compare what we staged (st.charges, captured when the item
			// was dropped into the trade window) with what the inventory row
			// currently holds.  Any mismatch points at a partial-stack split
			// the server failed to model (we read & stage the FULL row but the
			// client only intended to give number_in_stack).
			const int16_t db_charges_now = static_cast<int16_t>(Strings::ToInt(r.begin()[0]));
			LogInfo("[TrilogyZone] TradeGiveDIAG char={} npc={} trade_slot={} item={} "
			        "from_db={} staged_charges={} db_charges_now={} (deleting full row)",
			        s.char_id, npc->GetNPCTypeID(), i, st.item_id,
			        st.from_db_slot, (int)st.charges, (int)db_charges_now);

			taken[i] = database.CreateItem(st.item_id, st.charges);
			database.QueryDatabase(fmt::format(
			    "DELETE FROM inventory WHERE charid={} AND slotid={} AND itemid={}",
			    s.char_id, st.from_db_slot, st.item_id));
		}

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
	} else {
		// Non-quest NPC: server doesn't take or move anything; the client
		// returns the trade-window items to their visual source slot locally.
		// Refund any partial-pickup cursor rows back into their source DB
		// slot so server state matches the client's local return.
		RefundPartialCursorTradeItems(s);
	}

	for (auto& st : s.trade_items) st = Session::TradeStageItem{};
	s.trade_cp = s.trade_sp = s.trade_gp = s.trade_pp = 0;
	s.trade_npc_id = 0;

	uint8_t close_dummy = 0;
	SendApp(addr, port, s, ZN_OP_CloseTrade, &close_dummy, 0);
}

// ──────────────────────────────────────────────────────────────────────────
// Cursor-row refund (trade cancel / non-quest give)
//
// When a partial-stack pickup is staged into the NPC or PC trade window, the
// cursor row at DB slot 33 / 8000-8010 PERSISTS in DB until commit or cancel.
// On commit (quest NPC give, PC two-sided give), the row is consumed by the
// commit path — correct.  On CANCEL or GIVE-TO-NON-QUEST-NPC, the client
// locally returns the partial back to its visual source slot; without a
// refund, the server's cursor row would stay orphaned and drift from the
// client's view.
//
// Strategy per cursor-row trade item:
//   1. Source slot still holds the same item AND stackable: merge (cap at
//      StackSize, overflow stays in the cursor row).  Delete cursor row if
//      overflow is 0.
//   2. Source slot empty: UPDATE the cursor row's slotid back to the source
//      slot (puts the partial back exactly where the client returned it).
//   3. Source holds a different item now: leave the cursor row alone (would
//      otherwise trample real data).  Drift self-heals on zone-in.
//
// Whole-stack staging (original_source_db_slot < 0) needs no refund — the
// server never moved that row.  Skipped.
// ──────────────────────────────────────────────────────────────────────────
static void RefundOneCursorRow(uint32 char_id, uint32 item_id,
                               int cur_db, int src_db, int log_slot, const char* ctx)
{
	const bool is_cursor_row = (cur_db == 33) ||
	                           (cur_db >= 8000 && cur_db <= 8010);
	if (!is_cursor_row || src_db < 0 || item_id == 0) return;

	auto cur_q = database.QueryDatabase(fmt::format(
	    "SELECT `charges` FROM `inventory` "
	    "WHERE `charid`={} AND `slotid`={} AND `itemid`={}",
	    char_id, cur_db, item_id));
	if (!cur_q.Success() || cur_q.RowCount() == 0) return;
	const int16 cur_chg = static_cast<int16>(Strings::ToInt(cur_q.begin()[0]));

	auto src_q = database.QueryDatabase(fmt::format(
	    "SELECT `itemid`, `charges` FROM `inventory` "
	    "WHERE `charid`={} AND `slotid`={}",
	    char_id, src_db));
	const bool   src_present = (src_q.Success() && src_q.RowCount() > 0);
	const uint32 src_iid = src_present
	    ? static_cast<uint32>(Strings::ToInt(src_q.begin()[0])) : 0u;
	const int16  src_chg = src_present
	    ? static_cast<int16>(Strings::ToInt(src_q.begin()[1])) : int16{0};

	if (!src_present) {
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `slotid`={} WHERE `charid`={} AND `slotid`={}",
		    src_db, char_id, cur_db));
		LogInfo("[TrilogyZone] {} char={} slot={} item={} chg={} cursor_db={} src_db={} (source empty)",
		        ctx, char_id, log_slot, item_id, (int)cur_chg, cur_db, src_db);
		return;
	}

	if (src_iid != item_id) {
		LogInfo("[TrilogyZone] {} char={} slot={} item={} cursor_db={} src_db={} "
		        "src now holds item={} - leaving cursor row in place",
		        ctx, char_id, log_slot, item_id, cur_db, src_db, src_iid);
		return;
	}

	const EQ::ItemData* item = database.GetItem(item_id);
	const int stack_max = (item && item->StackSize > 0) ? item->StackSize : 1;
	const int total     = src_chg + cur_chg;

	if (total <= stack_max) {
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `charges`={} WHERE `charid`={} AND `slotid`={}",
		    total, char_id, src_db));
		database.QueryDatabase(fmt::format(
		    "DELETE FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    char_id, cur_db));
		LogInfo("[TrilogyZone] {} char={} slot={} item={} merged cursor_db={} ({}) "
		        "into src_db={} ({}) result={} (stack_max={})",
		        ctx, char_id, log_slot, item_id, cur_db, (int)cur_chg,
		        src_db, (int)src_chg, total, stack_max);
	} else {
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `charges`={} WHERE `charid`={} AND `slotid`={}",
		    stack_max, char_id, src_db));
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `charges`={} WHERE `charid`={} AND `slotid`={}",
		    total - stack_max, char_id, cur_db));
		LogInfo("[TrilogyZone] {} char={} slot={} item={} partial merge cursor_db={} ({}) "
		        "+ src_db={} ({}) result={} overflow={} (stack_max={})",
		        ctx, char_id, log_slot, item_id, cur_db, (int)cur_chg,
		        src_db, (int)src_chg, stack_max, total - stack_max, stack_max);
	}
}

// Resync the player's m_inv for any DB rows the refund just touched so engine
// reads (lore, CalcBonuses, etc.) stay coherent until the next action.
void TrilogyZoneServer::ResyncMInvForRefund(Session& s,
                                            const std::vector<int>& slots_to_sync)
{
	if (!s.trilogy_client) return;
	auto& inv = s.trilogy_client->GetInv();
	auto is_worn_slot = [](int slot) -> bool {
		return (slot >= 1 && slot <= 20) || slot == EQ::invslot::slotAmmo;
	};
	for (int db_slot : slots_to_sync) {
		if (db_slot < 0 || is_worn_slot(db_slot)) continue;
		auto* old = inv.PopItem(static_cast<int16>(db_slot));
		safe_delete(old);
		auto r = database.QueryDatabase(fmt::format(
		    "SELECT `itemid`,`charges`,`color` FROM `inventory` "
		    "WHERE `charid`={} AND `slotid`={}", s.char_id, db_slot));
		if (!r.Success() || r.RowCount() == 0) continue;
		auto row = r.begin();
		const uint32 iid = static_cast<uint32>(Strings::ToInt(row[0]));
		if (iid == 0) continue;
		const int16  ch  = static_cast<int16>(Strings::ToInt(row[1]));
		const uint32 col = static_cast<uint32>(Strings::ToInt(row[2]));
		EQ::ItemInstance* inst = database.CreateItem(iid, ch);
		if (!inst) continue;
		inst->SetColor(col);
		inv.PutItem(static_cast<int16>(db_slot), *inst);
		safe_delete(inst);
	}
}

void TrilogyZoneServer::RefundPartialCursorTradeItems(Session& s)
{
	std::vector<int> resync;
	for (int i = 0; i < 4; ++i) {
		auto& st = s.trade_items[i];
		if (st.item_id == 0) continue;
		if (st.original_source_db_slot < 0) continue;
		RefundOneCursorRow(s.char_id, st.item_id,
		                   st.from_db_slot, st.original_source_db_slot,
		                   i, "TradeRefund");
		resync.push_back(st.original_source_db_slot);
		resync.push_back(st.from_db_slot);
	}
	ResyncMInvForRefund(s, resync);
}

// PC-trade equivalent: same per-cursor-row refund logic across pc_trade_main.
// Called from PcTradeAbortBoth (and the abort branches inside HandleTradeGive
// PC commit failures) so a cancelled PC trade after partial pickups leaves
// the server's DB matching what the client locally restored.
void TrilogyZoneServer::RefundPartialCursorPcTradeItems(Session& s)
{
	std::vector<int> resync;
	for (int i = 0; i < 8; ++i) {
		auto& st = s.pc_trade_main[i];
		if (st.item_id == 0) continue;
		if (st.original_source_db_slot < 0) continue;
		RefundOneCursorRow(s.char_id, st.item_id,
		                   st.from_db_slot, st.original_source_db_slot,
		                   i, "PCTradeRefund");
		resync.push_back(st.original_source_db_slot);
		resync.push_back(st.from_db_slot);
	}
	ResyncMInvForRefund(s, resync);
}

void TrilogyZoneServer::CleanupOrphanedCursorTradeItems(Session& s)
{
	if (!s.trilogy_client) return;

	for (int i = 0; i < 4; ++i) {
		const auto& st = s.trade_items[i];
		if (st.item_id == 0) continue;
		// Partial-pickup materialized cursors are handled by
		// RefundPartialCursorTradeItems — skip so we don't double-touch.
		if (st.original_source_db_slot >= 0) continue;

		const bool is_cursor_row = (st.from_db_slot == 33) ||
		                           (st.from_db_slot >= 8000 && st.from_db_slot <= 8010);
		if (!is_cursor_row) continue;

		// Match on itemid too so we don't accidentally nuke an unrelated row
		// that happens to occupy the same slot after a race / desync.
		database.QueryDatabase(fmt::format(
		    "DELETE FROM `inventory` WHERE `charid`={} AND `slotid`={} AND `itemid`={}",
		    s.char_id, st.from_db_slot, st.item_id));

		// Keep m_inv in sync — pop the cursor entry the base engine may hold
		// so CheckLoreConflict / GetInv() reads don't lag the DB.  Only the
		// EQEmu-slotCursor mirror is relevant here; the 8000-8010 queue lives
		// only in DB, not m_inv.
		if (st.from_db_slot == 33) {
			auto& inv = s.trilogy_client->GetInv();
			if (auto* old = inv.PopItem(EQ::invslot::slotCursor)) safe_delete(old);
		}

		LogInfo("[TrilogyZone] TradeOrphanCleanup char={} slot_idx={} item={} "
		        "from_db={} — client already cleared cursor visually, DELETE row",
		        s.char_id, i, st.item_id, st.from_db_slot);
	}
}

void TrilogyZoneServer::HandleTradeCancel(const std::string& addr, int port, Session& s)
{
	if (!s.trilogy_client) return;

	// ── PC trade cancel ─────────────────────────────────────────────────────
	if (s.pc_trade_active) {
		Session* partner = FindSessionByEntityId(s.pc_trade_partner_id);
		PcTradeAbortBoth(s, partner, nullptr, "Your trade partner cancelled the trade.");
		LogInfo("[TrilogyZone] PCTrade cancelled by {}", s.char_name);
		return;
	}

	// ── NPC cancel ──────────────────────────────────────────────────────────
	// Refund any partial-pickup cursor rows so server state matches the
	// client's local return-to-source behaviour.  Must run BEFORE clearing
	// trade_items (the refund needs the staged from_db_slot + origin).
	RefundPartialCursorTradeItems(s);
	// Delete any full-item cursor stages the client never gave (client visually
	// cleared cursor on stage-in and v29c does NOT auto-restore on close).
	// Also runs before trade_items clear because it reads from_db_slot.
	CleanupOrphanedCursorTradeItems(s);

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

	// Diagnostic: log EVERY incoming MoveCoin so silent early-returns are
	// still visible in the log when we're working out v29c behaviour.
	LogInfo("[TrilogyZone] MoveCoin rx char={} from={} to={} ct1={} ct2={} amount={}",
	        s.char_name, from_slot, to_slot,
	        static_cast<int>(mc->cointype1), static_cast<int>(mc->cointype2),
	        static_cast<int>(mc->amount));

	// ── PC trade coin deposit / withdraw (trade slot = 3) ─────────────────
	// v29c sends OP_MoveCoin with to_slot==3 when the player drops coin into
	// the trade window and from_slot==3 when they pull it back out.  The
	// carried↔bank logic below doesn't know about the trade window — it
	// either silently early-returns (cursor↔trade) or vaporises the coin
	// (carried→trade: PP debited, never tracked).  Intercept here so the
	// coin lands in pc_trade_offer_* and the partner is notified via
	// OP_TradeCoins so their trade window paints the amount.
	if (s.pc_trade_active && (from_slot == 3 || to_slot == 3) &&
	    mc->cointype1 <= 3 && mc->amount > 0) {
		const uint32_t denom  = mc->cointype1; // source denomination
		const uint32_t amount = static_cast<uint32_t>(mc->amount);
		auto& pp = s.trilogy_client->GetPP();

		auto add_offer = [&](int dir) {
			auto& f = (denom == 0) ? s.pc_trade_offer_cp :
			          (denom == 1) ? s.pc_trade_offer_sp :
			          (denom == 2) ? s.pc_trade_offer_gp :
			                         s.pc_trade_offer_pp;
			if (dir > 0) f += amount;
			else         f  = (f >= amount) ? f - amount : 0;
		};
		auto adjust_pp = [&](int dir) {
			auto& f = (denom == 0) ? pp.copper :
			          (denom == 1) ? pp.silver :
			          (denom == 2) ? pp.gold   :
			                         pp.platinum;
			if (dir > 0) f += amount;
			else         f  = (f > amount) ? f - amount : 0;
		};

		if (to_slot == 3) {
			// Depositing into trade window.
			add_offer(+1);
			// Source = carried (1) → debit PP now.
			// Source = cursor  (0) → PP was already debited by the prior
			//                        MoveCoin(carried→cursor) pickup, no change.
			if (from_slot == 1) adjust_pp(-1);

			// Notify partner so their window paints our offer.
			Session* partner = FindSessionByEntityId(s.pc_trade_partner_id);
			if (partner && partner->trilogy_client) {
				uint8_t out[22];
				std::memset(out, 0, sizeof(out));
				const uint32_t partner_id =
				    static_cast<uint32_t>(partner->trilogy_client->GetID());
				std::memcpy(out + 0, &partner_id, 4);
				out[4] = static_cast<uint8_t>(denom);
				const uint16_t magic = 0x4fD2;
				std::memcpy(out + 5, &magic, 2);
				std::memcpy(out + 8, &amount, 4);
				SendApp(partner->source_addr, partner->source_port, *partner,
				        ZN_OP_TradeCoins, out, static_cast<uint32_t>(sizeof(out)));
			}

			LogInfo("[TrilogyZone] PCTrade coin deposit char={} denom={} amount={} from={} "
			        "(offer cp={} sp={} gp={} pp={})",
			        s.char_name, denom, amount, from_slot,
			        s.pc_trade_offer_cp, s.pc_trade_offer_sp,
			        s.pc_trade_offer_gp, s.pc_trade_offer_pp);
		} else { // from_slot == 3 — withdrawing from trade window
			add_offer(-1);
			// Dest = carried (1) → credit PP now.
			// Dest = cursor  (0) → coin sits on cursor visually; no PP change.
			if (to_slot == 1) adjust_pp(+1);

			LogInfo("[TrilogyZone] PCTrade coin withdraw char={} denom={} amount={} to={} "
			        "(offer cp={} sp={} gp={} pp={})",
			        s.char_name, denom, amount, to_slot,
			        s.pc_trade_offer_cp, s.pc_trade_offer_sp,
			        s.pc_trade_offer_gp, s.pc_trade_offer_pp);
		}

		s.trilogy_client->Save();
		s.last_copper = pp.copper; s.last_silver = pp.silver;
		s.last_gold   = pp.gold;   s.last_platinum = pp.platinum;
		s.money_synced = true;
		return;
	}

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
	// Diagnostic — investigating missing cost display in trainer window.
	// EQClassic's ClassTrain_Struct is 148B (uint8 highesttrain[73] + unknowns);
	// EQMacEmuTrilogy's OldGMTrainee_Struct is 244B and includes a `float greed`
	// price modifier + language[32] + trailing ending block that modern EQEmu
	// preserves via memcpy on Titanium's 448B struct.  If v29c actually expects
	// the 244B layout, our 148B reply would leave the client without a valid
	// greed field → no cost displayed.  Log the actual size so we can confirm.
	if (plen != sizeof(Trilogy::structs::ClassTrain_Struct)) {
		LogInfo("[TrilogyZone] ClassTraining REQ size={} (our struct={}) — extra {} bytes",
		        plen, sizeof(Trilogy::structs::ClassTrain_Struct),
		        plen - sizeof(Trilogy::structs::ClassTrain_Struct));
	} else {
		LogInfo("[TrilogyZone] ClassTraining REQ size={} (matches struct)", plen);
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
	//
	// CRITICAL: echo the client's own player_id from the request unchanged.
	// v29c uses this field to match the response against its OWN self-ID (the
	// wire spawn_id it was given on zone-in — for character Nekoto that's
	// 16616, not the EQEmu GetID() which is a small entity index).  If the
	// echoed value doesn't match the client's expected self-ID, subsystems
	// like the per-skill cost/price display in the trainer window silently
	// fail to render — the window itself opens (that path only needs the
	// unknown[32] magic bytes), but the cost text stays blank.  Same trap as
	// documented in TrilogyClient::HandleClickObjectAction (trilogy_client.cpp
	// ~L820, "v29c client only opens the station UI when the packet's
	// player_id matches its own self-ID").  EQClassic's ProcessOP_ClassTraining
	// side-steps this by modifying the inbound packet in-place and re-queuing
	// it (echoing both npcid and playerid).  We do the same explicitly.
	Trilogy::structs::ClassTrain_Struct reply{};
	reply.npcid    = req->npcid;
	reply.playerid = req->playerid;

	// Skill caps — highesttrain[i] is the max value this trainer can raise
	// skill i to at the character's CURRENT level.  The v29c client uses this
	// as the denominator for the trainer window's tier bar
	// ("awful/very bad/average/master") — ratio = pp.skills[i] / highesttrain[i].
	// Sending the absolute class cap (MaxLevel ~250) makes the ratio stay tiny
	// forever and the tier never visibly advances as the player trains, which
	// looks like the UI is stuck.  EQClassic's ProcessOP_ClassTraining
	// (Zone/Source/client_process.cpp:5612) passes GetLevel() to CheckMaxSkill,
	// producing a per-level cap (~30 at level 5) that the tier bar can move
	// meaningfully against.  Match that.
	//
	// Iterate only the 73 indices the wire struct holds; the Trilogy enum is
	// densely packed in the same order EQEmu uses for these indices, so a
	// direct id→id mapping works.  Skills the class can never learn
	// (CanHaveSkill==false) get 0 → hidden from the window.
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
		                               s.trilogy_client->GetLevel()));
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

	// Greed / price-modifier float — the v29c client reads a float from what
	// modern EQEmu's OPGMTraining treats as "last 40 bytes = trailing metadata
	// block" (see client_process.cpp:1674-1679 — a `memcpy(&outapp->pBuffer
	// [outapp->size-40], ending, 40)` where ending starts with a float ≈ 1.08).
	// In our 148 B response that puts greed at offset 108 (= unknown[27..30]).
	//
	// Symptom before this fix: the per-skill cost widget rendered "No charge"
	// at every skill level because our all-1s pattern in unknown[] made the
	// client read greed = 0x01010101 little-endian = ~2.35e-38 (subnormal) ≈ 0,
	// so `displayed_cost = client_hardcoded_formula * greed` collapsed to 0.
	//
	// Use Titanium's exact magic bytes (0x3F8A8734 = ~1.082) instead of a
	// clean 1.0f — 1.0f encodes as {0x00,0x00,0x80,0x3F} which introduces
	// zero bytes into unknown[], and Wizzel's EQClassic comment warns "one of
	// these are important or the trainer wont open the training window".
	// 0x3F8A8734 has no zero bytes, so it satisfies the non-zero-byte
	// heuristic while giving the client a valid ~1.0 multiplier.
	{
		static constexpr uint8_t kGreedBytes[4] = { 0x34, 0x87, 0x8A, 0x3F };
		auto* raw = reinterpret_cast<uint8_t*>(&reply);
		static_assert(sizeof(reply) >= 40, "ClassTrain_Struct too small for greed placement");
		std::memcpy(raw + (sizeof(reply) - 40), kGreedBytes, sizeof(kGreedBytes));
	}

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
// Inspect (right-click another player) — pure client-to-client relay.
// Wire model (EQClassic/Zone/Source/client_process.cpp:4590-4618):
//   Client A sends 8B OP_InspectRequest {target=B, player=A} → server
//   forwards to B → B's client builds a 1044B OP_InspectAnswer locally
//   from its own equipment + inspect text → server relays back to A.
// The 1036B payload is opaque to the server.  For Phase 1 we only wire
// Trilogy ↔ Trilogy; bots + modern-client cross-inspect are follow-ups
// that need the payload layout decoded from a Zone:TrilogyInspectDebug
// capture of a real v29c answer.
// ============================================================
void TrilogyZoneServer::HandleInspectRequest(const std::string& addr, int port, Session& s,
                                             const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < 8) {
		LogInfo("[TrilogyZone] Inspect request: short payload {} (expected 8) char={}",
		        plen, s.char_name);
		return;
	}

	uint32_t target_id = 0, player_id = 0;
	std::memcpy(&target_id, payload + 0, 4);
	std::memcpy(&player_id, payload + 4, 4);

	// Look up target via FindSessionByEntityId FIRST — it matches on both
	// EQEmu entity id AND wire player_spawn_id, which is required for
	// self-inspect: the wire target_id comes from the client's self-view
	// (= player_spawn_id, e.g. 16616), NOT the EQEmu entity id (= 232),
	// and entity_list.GetMob() only indexes EQEmu ids.  Using GetMob first
	// used to make self-inspect silently drop ("no entity 16616 in zone").
	Session* partner = FindSessionByEntityId(static_cast<uint16_t>(target_id));
	if (partner && partner->trilogy_client) {
		// Trilogy ↔ Trilogy: relay the raw 8B so the target's client builds
		// + returns an OP_InspectAnswer, which we relay back in HandleInspectAnswer.
		//
		// Stash the target id AS THIS CLIENT SEES IT.  When the answer comes
		// back the responder will have filled PlayerID with THEIR own-view id
		// (Curwen self=16417) — but this session's client only knows Curwen
		// as target_id (=221 in the 2026-08-08 repro).  Without the rewrite
		// the requester's client silently drops the answer and the window
		// never opens.
		s.pending_inspect_target_id = static_cast<uint16_t>(target_id);
		const bool is_self = (partner == &s);
		LogInfo("[TrilogyZone] Inspect request RELAY: {} (entity {}) inspects {} (entity {}){}",
		        s.char_name, player_id, partner->char_name, target_id,
		        is_self ? " [SELF]" : "");
		SendApp(partner->source_addr, partner->source_port, *partner,
		        ZN_OP_InspectRequest, payload, 8);

		// Notify the target — v29c does NOT auto-generate this
		// notification server-side (unlike Titanium+, per modern EQEmu
		// client.cpp:6227 comment).  The initiator gets a client-native
		// message once the answer is displayed correctly (verified 2026-08-08
		// after the doubled-target header fix), so no server-side self
		// message needed.  Suppress for self-inspect.
		if (!is_self) {
			partner->trilogy_client->Message(Chat::White,
			    "%s is looking at your equipment...", s.char_name);
		}
		return;
	}

	// Fallback lookup: target may be a non-Trilogy entity (bot/NPC/modern PC).
	Mob* target = entity_list.GetMob(static_cast<uint16_t>(target_id));
	if (!target) {
		LogInfo("[TrilogyZone] Inspect request: no entity {} in zone (from char={})",
		        target_id, s.char_name);
		return;
	}

	// ── Bot inspect ──────────────────────────────────────────────────────
	// Bots have no client to build the answer, so build the v29c
	// OP_InspectAnswer server-side from the bot's equipment + persisted
	// inspect message and send it directly to the requester.
	//
	// v29c 1036B payload layout (verified from PC captures 2026-08-08):
	//   +0x000..+0x2BF  22 slots × 32B null-terminated ASCII item names
	//                    (wire slot order: 0=charm .. 20=waist, 21=ammo —
	//                    powerSource has no wire slot in v29c so we map
	//                    wire[21] → EQEmu slotAmmo(22))
	//   +0x2C0..+0x2EB  22 × uint16 icon ids, 0xFFFF = no icon
	//   +0x2EC..+0x40B  288B About-Me / inspect message text
	// Header (8B before body): both TargetID and PlayerID = bot's entity
	// id (the same value all clients see for bots, unlike PCs — bots have
	// no dual-wire-id problem).
	if (target->IsBot()) {
		Bot* bot = target->CastToBot();
		std::vector<uint8_t> pkt(1044, 0);
		const uint32_t bot_id = static_cast<uint32_t>(bot->GetID());
		std::memcpy(pkt.data() + 0, &bot_id, 4);
		std::memcpy(pkt.data() + 4, &bot_id, 4);

		uint8_t* body = pkt.data() + 8;
		uint16_t* icons = reinterpret_cast<uint16_t*>(body + 0x2C0);
		for (int i = 0; i < 22; ++i) icons[i] = 0xFFFF;

		// wire slot → EQEmu invslot mapping.  Identical for 0..20; slot 21
		// (ammo on the wire) skips the modern powerSource=21 gap.
		static constexpr int16 kSlotMap[22] = {
			EQ::invslot::slotCharm,     EQ::invslot::slotEar1,
			EQ::invslot::slotHead,      EQ::invslot::slotFace,
			EQ::invslot::slotEar2,      EQ::invslot::slotNeck,
			EQ::invslot::slotShoulders, EQ::invslot::slotArms,
			EQ::invslot::slotBack,      EQ::invslot::slotWrist1,
			EQ::invslot::slotWrist2,    EQ::invslot::slotRange,
			EQ::invslot::slotHands,     EQ::invslot::slotPrimary,
			EQ::invslot::slotSecondary, EQ::invslot::slotFinger1,
			EQ::invslot::slotFinger2,   EQ::invslot::slotChest,
			EQ::invslot::slotLegs,      EQ::invslot::slotFeet,
			EQ::invslot::slotWaist,     EQ::invslot::slotAmmo,
		};

		for (int wire_slot = 0; wire_slot < 22; ++wire_slot) {
			const EQ::ItemInstance* inst = bot->GetBotItem(static_cast<uint16>(kSlotMap[wire_slot]));
			if (!inst) continue;
			const EQ::ItemData* item = inst->GetItem();
			if (!item) continue;
			char* name_dst = reinterpret_cast<char*>(body + wire_slot * 32);
			std::strncpy(name_dst, item->Name, 31); // 31 chars + NUL
			icons[wire_slot] = static_cast<uint16_t>(item->Icon);
		}

		// About-Me: from bot_inspect_messages via Bot::GetInspectMessage
		// (loaded at bot spawn — bot.cpp:247).  288B max, always NUL-terminate.
		const auto& msg = bot->GetInspectMessage();
		if (msg.text[0]) {
			std::strncpy(reinterpret_cast<char*>(body + 0x2EC), msg.text, 287);
		}

		LogInfo("[TrilogyZone] Bot inspect: {} inspects bot {} (entity {}) — built server-side",
		        s.char_name, bot->GetCleanName(), bot_id);
		SendApp(addr, port, s, ZN_OP_InspectAnswer, pkt.data(), 1044);
		return;
	}

	// Other non-Trilogy targets (modern-client PC, non-bot NPC) — not implemented.
	const char* kind =
	    target->IsClient() ? "client" :
	    target->IsNPC()    ? "npc"    : "other";
	LogInfo("[TrilogyZone] Inspect request DROP: {} inspects entity {} (kind={}, name={}) — "
	        "no Trilogy session; modern-client inspect not implemented",
	        s.char_name, target_id, kind, target->GetCleanName());
}

void TrilogyZoneServer::HandleInspectAnswer(const std::string& /*addr*/, int /*port*/, Session& s,
                                            const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	// Expected 1044 bytes: 8B header + 1036B opaque payload.
	if (plen < 8) {
		LogInfo("[TrilogyZone] Inspect answer: short payload {} char={}", plen, s.char_name);
		return;
	}

	uint32_t requester_id = 0, responder_id = 0;
	std::memcpy(&requester_id, payload + 0, 4); // TargetID on the answer = the requester
	std::memcpy(&responder_id, payload + 4, 4); // PlayerID on the answer = the responder (us)

	// Hex-dump the 1036B payload the FIRST time only.  v29c re-sends
	// OP_InspectRequest ~2/sec while the window is open — logging every
	// answer floods the zone log (237 × 66 = 15,642 lines / 2 min in the
	// original repro was enough to make the zone unresponsive).  One
	// capture is all we need to reverse-engineer the payload layout.
	if (!s.inspect_captured) {
		const uint8_t* body = payload + 8;
		const uint32_t body_len = plen - 8;
		LogInfo("[TrilogyZone] Inspect answer CAPTURE from {} (entity {}) to entity {} — "
		        "payload {} bytes:",
		        s.char_name, responder_id, requester_id, body_len);
		for (uint32_t off = 0; off < body_len; off += 16) {
			char hex[64] = {};
			char asc[17] = {};
			char* hp = hex;
			for (uint32_t j = 0; j < 16; ++j) {
				if (off + j < body_len) {
					const uint8_t b = body[off + j];
					hp += std::snprintf(hp, hex + sizeof(hex) - hp, "%02x ", b);
					asc[j] = (b >= 0x20 && b < 0x7f) ? static_cast<char>(b) : '.';
				} else {
					hp += std::snprintf(hp, hex + sizeof(hex) - hp, "   ");
					asc[j] = ' ';
				}
			}
			asc[16] = '\0';
			LogInfo("[TrilogyZone]   +{:04x}  {} |{}|", off, hex, asc);
		}
		s.inspect_captured = true;
	}

	// TODO(inspect-phase2): once the CAPTURE dump identifies the text-field
	// offset, snapshot the inspect message to the DB here via
	// database.SaveCharacterInspectMessage(s.char_id, ...).  Leaving the hook
	// unwired for now so we don't scribble unknown bytes into character_inspect_messages.

	// Relay to the requester.  Per EQClassic model, forward the packet
	// including the opaque payload — BUT rewrite both header ids so the
	// v29c client's UI dispatches to the correct window.
	//
	// The responder built the answer using the EQClassic swap convention
	// (TargetID = requester, PlayerID = responder self-view).  Two problems
	// for the requester's client after receipt:
	//   1. PlayerID = responder's own-view (Curwen self=16417) but this
	//      requester's client only knows the responder under a different
	//      id (Nekoto sees Curwen as 221 in the 2026-08-08 repro).  Without
	//      fixing this, the client silently drops the answer (unknown entity)
	//      and the inspect window never opens.
	//   2. TargetID = requester's own id (Nekoto self=16616), which the
	//      v29c client's fullscreen/semi-transparent UI reads as "this
	//      answer describes ME" and opens the character sheet instead of
	//      the dedicated inspect-other window.  (The old stone UI doesn't
	//      check TargetID this way — same relay opens the correct window
	//      there.)
	//
	// Fix: rewrite the header into "displayed / inspector" order that both
	// UIs handle correctly:
	//   payload[+0] TargetID = displayed entity, in REQUESTER'S VIEW
	//                        = requester->pending_inspect_target_id (=221)
	//   payload[+4] PlayerID = the requester itself, self-view
	//                        = original answer's TargetID (=requester_id, 16616)
	// For self-inspect both values equal the requester's self id, so the
	// UI opens the char sheet — which is the correct behavior for self.
	Session* requester = FindSessionByEntityId(static_cast<uint16_t>(requester_id));
	if (!requester || !requester->trilogy_client) {
		LogInfo("[TrilogyZone] Inspect answer: no Trilogy session for requester entity {} "
		        "(from char={}) — dropping",
		        requester_id, s.char_name);
		return;
	}

	std::vector<uint8_t> patched(payload, payload + plen);
	if (requester->pending_inspect_target_id != 0) {
		// Set BOTH fields to the responder's id AS THE REQUESTER SEES IT.
		// Empirical: on the fullscreen/semi-transparent UI, whichever field
		// the client uses for character-name lookup was resolving to the
		// requester's OWN entity (because we had put requester_id in
		// PlayerID), so the inspect window showed "Nekoto" not "Curwen".
		// Setting both to the target-from-requester-view keeps any
		// interpretation (route id, display id, name lookup id) pointed
		// at the correct entity in the requester's local table.  For
		// self-inspect this collapses to (self,self) same as before.
		const uint32_t tid = requester->pending_inspect_target_id;
		std::memcpy(patched.data() + 0, &tid, 4);
		std::memcpy(patched.data() + 4, &tid, 4);
		LogInfo("[TrilogyZone] Inspect answer relay: rewrote header target={} player={} "
		        "(was target={} player={}) for requester {} (responder {})",
		        tid, tid, requester_id, responder_id,
		        requester->char_name, s.char_name);
	}

	SendApp(requester->source_addr, requester->source_port, *requester,
	        ZN_OP_InspectAnswer, patched.data(), plen);
}

// ============================================================
// HandleSurname — /surname command from a Trilogy client.
//
// Wire in : Trilogy::structs::Surname_Struct (56 B) at opcode 0xc421.
// Wire out: relies on Client::ChangeLastName() broadcasting OP_GMLastName,
//           which TrilogyClient::TranslateAndSend translates to the 94 B
//           GMSurname_Struct at wire opcode 0x6e21 for every nearby Trilogy
//           client (including the sender).  The sender's own OP_Surname
//           accept-echo is emitted from Handle_OP_Surname via FastQueuePacket
//           and is also translated to 0xc421 in TranslateAndSend.
//
// We do a light translation of the 56 B Trilogy struct into the 100 B EQEmu
// struct so all the shared validation (level 20 gate, 7-day cooldown, name
// filter, DB persistence) runs unchanged out of Client::Handle_OP_Surname.
// ============================================================
void TrilogyZoneServer::HandleSurname(const std::string& /*addr*/, int /*port*/, Session& s,
                                      const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::Surname_Struct)) {
		LogInfo("[TrilogyZone] /surname: short payload {} from char={}", plen, s.char_name);
		return;
	}

	const auto* ts = reinterpret_cast<const Trilogy::structs::Surname_Struct*>(payload);

	EQApplicationPacket app(OP_Surname, sizeof(::Surname_Struct));
	memset(app.pBuffer, 0, sizeof(::Surname_Struct));
	auto* es = reinterpret_cast<::Surname_Struct*>(app.pBuffer);

	// Trust the session's server-side name over the client-provided one — v29c
	// only sends the first 15 chars and it isn't consulted server-side anyway.
	strn0cpy(es->name, s.trilogy_client->GetName(), sizeof(es->name));
	es->unknown0064 = 0;
	strn0cpy(es->lastname, ts->Surname, sizeof(es->lastname));

	LogInfo("[TrilogyZone] /surname: {} -> '{}'", s.char_name, es->lastname);
	s.trilogy_client->Handle_OP_Surname(&app);
}

// ============================================================
// HandleSocialText — the chat line half of a social (/bow, /wave, /emote).
//
// Wire in : opcode 0x1520, a bare NUL-terminated string holding only the
//           PREDICATE — " bows." — with its own leading space.  The actor's
//           name is the server's job to prepend.
//
// EQClassic's ProcessOP_Social_Text (client_process.cpp:4203) does exactly
// that and nothing else:
//     cptr += sprintf(cptr, "%s", GetName());
//     cptr += sprintf(cptr, "%s", pApp->pBuffer);
//     entity_list.QueueCloseClients(this, outapp, true, 100);
//
// We route the equivalent through Client::Handle_OP_Emote instead of relaying
// by hand.  That handler is the same code with the same ancestry — it prepends
// GetName() and calls QueueCloseClients with ignore_sender=true (both even
// carry the identical commented-out `replacestr` block for rewriting the
// target's copy to "you", abandoned in both trees).  Going through it buys
// cross-client delivery: a Titanium observer gets a native OP_Emote while
// Trilogy observers get 0x1520 back via TranslateAndSend, instead of the
// Trilogy-only relay a literal port would have given us.
//
// Range differs deliberately: EQClassic hardcodes 100 units, EQEmu uses
// RuleI(Range, Emote).  Keeping the rule means one knob for every client.
//
// Payload offset: EQClassic's Emote_Text declares `int16 unknown1` before
// `message`, but its live "new client method" reads and writes the string from
// byte 0, overwriting that field; the +2 form is commented out as the "old
// client method".  We follow the live path and fall back to +2 only when byte 0
// is NUL but a string starts at +2, logging which layout was used the first
// time per session so a mismatch shows up immediately rather than as silence.
// ============================================================
void TrilogyZoneServer::HandleSocialText(const std::string& /*addr*/, int /*port*/, Session& s,
                                          const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client || plen == 0) return;

	// Locate the predicate string, tolerating either layout.
	const char* body   = reinterpret_cast<const char*>(payload);
	uint32_t    avail  = plen;
	const char* layout = "offset0";
	if (payload[0] == '\0' && plen > 2 && payload[2] != '\0') {
		body   = reinterpret_cast<const char*>(payload + 2);
		avail  = plen - 2;
		layout = "offset2";
	}

	// Bound the copy: the buffer is not guaranteed NUL-terminated on the wire,
	// and Handle_OP_Emote wants a fixed-size Emote_Struct.
	char msg[512] = {};
	uint32_t n = 0;
	while (n < avail && n < sizeof(msg) - 1 && body[n] != '\0') {
		msg[n] = body[n];
		++n;
	}
	msg[n] = '\0';
	if (n == 0) return;

	if (!s.social_text_logged) {
		s.social_text_logged = true;
		LogInfo("[TrilogyZone] social text: char=[{}] layout={} plen={} predicate=['{}']",
		        s.char_name, layout, plen, msg);
	}

	EQApplicationPacket app(OP_Emote, sizeof(::Emote_Struct));
	memset(app.pBuffer, 0, sizeof(::Emote_Struct));
	auto* es = reinterpret_cast<::Emote_Struct*>(app.pBuffer);
	es->type = 0;
	strn0cpy(es->message, msg, sizeof(es->message));

	s.trilogy_client->Handle_OP_Emote(&app);
}

// ============================================================
// HandleSocialAction — the body-animation half of a social.
//
// Wire in : opcode 0x9f20, the same 12-byte shape we already emit outbound for
//           OP_Animation (Trilogy Attack_Struct): int32 spawn_id at 0, the
//           animation id at byte 4.  EQClassic calls it Social_Action_Struct
//           (`uint8 unknown1[4]; uint8 action; uint8 unknown2[7]`) — identical
//           layout, which is why one opcode carries both melee swings and /bow.
//
// EQClassic relays the client's packet verbatim and does nothing else:
//     case OP_Social_Action: entity_list.QueueCloseClients(this, app, true);
//
// We re-emit as OP_Animation rather than echoing the raw bytes, so the
// animation reaches Titanium observers too; TranslateAndSend turns it back into
// 0x9f20 for Trilogy observers via HandleAnimation, which already stamps the
// 0x80/0x3F trailer EQClassic's DoAnim writes.
//
// Built by hand instead of calling Mob::DoAnim because DoAnim hardcodes
// ignore_sender=false.  EQClassic passes true here: the emoting client plays
// its own animation locally, and echoing it back re-triggers the gesture.
// ============================================================
void TrilogyZoneServer::HandleSocialAction(const std::string& /*addr*/, int /*port*/, Session& s,
                                            const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < 5) {
		LogInfo("[TrilogyZone] social action: short payload {} from char=[{}]", plen, s.char_name);
		return;
	}

	const uint8_t action = payload[4];
	if (action == 0) return;

	if (!s.social_action_logged) {
		s.social_action_logged = true;
		LogInfo("[TrilogyZone] social action: char=[{}] plen={} anim={}",
		        s.char_name, plen, static_cast<int>(action));
	}

	auto outapp = new EQApplicationPacket(OP_Animation, sizeof(::Animation_Struct));
	auto* a = reinterpret_cast<::Animation_Struct*>(outapp->pBuffer);
	a->spawnid = s.trilogy_client->GetID();
	a->action  = action;
	a->speed   = 10; // DoAnim's default when the caller passes 0

	entity_list.QueueCloseClients(
		s.trilogy_client, /* sender          */
		outapp,           /* packet          */
		true,             /* ignore sender   */
		RuleI(Range, Anims),
		0,                /* skip this mob   */
		true              /* ack required    */
	);

	safe_delete(outapp);
}

// ============================================================
// HandleRezzAnswer — corpse owner clicked yes/no on the resurrection popup.
//
// Wire in : Trilogy::structs::Resurrect_Struct (160B) at opcode 0x9b21.
// Wire out: on accept, OP_RezzComplete (0xec21, same 160B struct) so the v29c
//           client tears down its pending-rez state (parity with EQClassic
//           ProcessOP_RezzAnswer at Zone/Source/client_process.cpp:6654).
//
// Server-side flow is the modern one: translate to the 228B EQEmu
// Resurrect_Struct and dispatch through Client::Handle_OP_RezzAnswer, which
// runs OPRezzAnswer (XP restore, MovePC to corpse coords, worldserver
// route-back of OP_RezzComplete so the corpse's zone marks it rezzed).
// Trilogy has no hover-rez — the player is either at bind or in another zone,
// so MovePC will drive either an OP_TeleportPC (same-zone) or an
// OP_RequestClientZoneChange (cross-zone), both of which already have Trilogy
// translators (see project_trilogy_skyshrine_pads memory).
//
// zone_id is looked up from the incoming zoneName via zone_store; server-side
// PendingRezzXP/etc. was stashed by WorldServer::HandleMessage:ServerOP_RezzPlayer
// when the popup was originally shown.
// ============================================================
void TrilogyZoneServer::HandleRezzAnswer(const std::string& addr, int port, Session& s,
                                          const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::Resurrect_Struct)) {
		LogInfo("[TrilogyRezz] OP_RezzAnswer short payload {} from char={}", plen, s.char_name);
		return;
	}

	const auto* rin = reinterpret_cast<const Trilogy::structs::Resurrect_Struct*>(payload);

	EQApplicationPacket app(OP_RezzAnswer, sizeof(::Resurrect_Struct));
	memset(app.pBuffer, 0, sizeof(::Resurrect_Struct));
	auto* rout = reinterpret_cast<::Resurrect_Struct*>(app.pBuffer);

	// Look up zone_id from zoneName; v29c echoes whatever we sent on the outbound
	// popup so this is the caster's zone (where the corpse is).
	char zshort[17] = {};
	strn0cpy(zshort, rin->zoneName, sizeof(zshort));
	rout->zone_id     = static_cast<uint16_t>(ZoneID(zshort));
	rout->instance_id = 0;

	rout->y       = rin->y;
	rout->x       = rin->x;
	rout->z       = rin->z;
	rout->spellid = rin->spellID;
	rout->action  = rin->action;

	strn0cpy(rout->your_name,   rin->targetName, sizeof(rout->your_name));
	strn0cpy(rout->rezzer_name, rin->casterName, sizeof(rout->rezzer_name));
	strn0cpy(rout->corpse_name, rin->corpseName, sizeof(rout->corpse_name));

	LogInfo("[TrilogyRezz] OP_RezzAnswer from {} action={} spell={} zone={}({}) corpse='{}'",
	        s.char_name,
	        rout->action ? "ACCEPT" : "DECLINE",
	        rout->spellid,
	        zshort,
	        rout->zone_id,
	        rout->corpse_name);

	s.trilogy_client->Handle_OP_RezzAnswer(&app);

	// EQClassic client_process.cpp:6654 sends OP_RezzComplete back to the
	// corpse owner on accept so the v29c client dismisses the pending-rez UI.
	// Modern EQEmu never queues this to the client (the world route-back at
	// worldserver.cpp:937 is a corpse-zone bookkeeping message, not user
	// visible), so we synthesize it here for wire parity.
	if (rin->action == 1) {
		SendApp(addr, port, s, ZN_OP_RezzComplete, payload, plen, true);
	}
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

	// EQClassic parity: SendZoneSpawnsBulk (Zone/Source/EntityList.cpp:913)
	// splits the bulk into chunks of at most 100 NewSpawn_Struct entries per
	// OP_ZoneSpawns packet.  v29c has a per-packet processing cap somewhere
	// past that; a single mega-packet (our prior behavior) causes entries
	// beyond the cap to be silently dropped by the client — the mob is
	// server-alive but invisible.  Rule-gate the split so it can be reverted
	// at runtime via `#reloadrules` if it regresses.
	static constexpr size_t kMaxSpawnsPerPacket = 100;
	const bool split_enabled = RuleB(Zone, TrilogyZoneSpawnsSplit);

	// Build raw NewSpawn_Struct[] array (168 bytes per entry: NPCs + players + corpses).
	std::vector<uint8_t> raw;
	const size_t reserve_hint = split_enabled
	                                ? kMaxSpawnsPerPacket
	                                : (npc_map.size() + client_map.size() + corpse_map.size());
	raw.reserve(reserve_hint * sizeof(Trilogy::structs::NewSpawn_Struct));

	const auto& bot_list   = entity_list.GetBotList();

	uint32_t sent          = 0; // cumulative across all chunks (bulk_idx for diag)
	uint32_t chunk_entries = 0; // entries in the currently-building chunk
	uint32_t chunk_packets = 0; // number of 6121 packets emitted so far

	auto flush_chunk = [&]() {
		if (chunk_entries == 0) return;

		uint32_t max_clen = EQ::EstimateDeflateBuffer(static_cast<uint32_t>(raw.size()));
		std::vector<uint8_t> cbuf(max_clen + 4, 0); // +4 for encrypt alignment
		uint32_t clen = EQ::DeflateData(
			reinterpret_cast<const char*>(raw.data()), static_cast<uint32_t>(raw.size()),
			reinterpret_cast<char*>(cbuf.data()), max_clen
		);
		if (clen == 0) {
			LogError("[TrilogyZone] SendZoneSpawns: deflate failed (chunk {} with {} entries)",
			         chunk_packets + 1, chunk_entries);
			raw.clear();
			chunk_entries = 0;
			return;
		}
		// Pad to multiple of 4 (EncryptZoneSpawnPacket operates on int32 values)
		while (clen % 4 != 0) cbuf[clen++] = 0;
		EncryptZoneSpawnPacket(cbuf.data(), clen);
		++chunk_packets;
		LogInfo("[TrilogyZone] SendZoneSpawns chunk {}: {} entries → raw={} compressed={} (~{} fragments)",
		        chunk_packets, chunk_entries, raw.size(), clen, clen >> 9);
		SendApp(addr, port, s, ZN_OP_ZoneSpawns, cbuf.data(), clen);
		raw.clear();
		chunk_entries = 0;
	};

	auto append_entry = [&](const Trilogy::structs::NewSpawn_Struct& ns_ref) {
		const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns_ref);
		raw.insert(raw.end(), p, p + sizeof(ns_ref));
		++chunk_entries;
		++sent;
		if (split_enabled && chunk_entries >= kMaxSpawnsPerPacket) flush_chunk();
	};
	for (const auto& kv : npc_map) {
		NPC* npc = kv.second;
		if (!npc) continue;

		Trilogy::structs::NewSpawn_Struct ns{};
		memset(&ns, 0, sizeof(ns));
		// ns.ns_unknown1 = 0 (padding, ignored by client)
		Trilogy::structs::Spawn_Struct& sp = ns.spawn;

		sp.size      = npc->GetSize();
		if (sp.size <= 0.0f) sp.size = 6.0f;
		// EXPERIMENT 2026-06-27: 3× bump on NPC spawn-time walk/run, paired
		// with the matching 2× anim multiplier bump in EncodeTrilogyAnim.
		// Rationale (EQClassic comparison):
		//   - EQClassic derives server motion FROM the anim byte:
		//       animation = walkspeed × 4   (walking)
		//       animation = runspeed  × 7   (running)
		//       NWUPS     = animation × 2.3/5
		//     so server speed and client-render speed match by construction.
		//   - EQEmu does the opposite: server moves at calibrated rates
		//     (10 u/s walk, 29 u/s run) and we tried to encode that into a
		//     byte v29c interprets at ~5 u/s — server outran client render,
		//     producing the steady-state forward snap.
		//   - v29c also uses spawn-time walkspeed/runspeed as the
		//     walk-vs-run animation cycle THRESHOLD (approximately
		//     anim_byte > walkspeed × 4 ⇒ run cycle).  Bumping spawn
		//     walkspeed raises that threshold so the higher anim bytes
		//     we now send stay in walk cycle for walking NPCs.
		// 3× is the EQClassic-equivalent calibration scaled for EQEmu's
		// faster server speeds.  Mob's server-side walkspeed/runspeed are
		// untouched (gameplay/Titanium unaffected); only the v29c wire
		// representation changes.
		// 2026-06-27: dialed walkspeed bump from 3× → 1.5×, runspeed
		// stays at 1×.  Reasoning: at 3× (walkspeed=1.35) any byte we
		// sent registered as walk cycle in v29c — looks like v29c locks
		// to walk cycle when walkspeed exceeds some upper bound rather
		// than scaling a linear threshold.  1.5× lands walkspeed at 0.675,
		// just under EQClassic's standard 0.7 where their byte=8 reliably
		// produces run cycle.  Walk-cycle byte=4 still stays well under
		// any threshold so walking animation should remain correct.
		sp.walkspeed = ToTrilogySpeed(npc->GetBaseWalkspeed()) * 1.5f;
		sp.runspeed  = ToTrilogySpeed(npc->GetBaseRunspeed());
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(npc->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(npc->GetY());
		sp.x_pos     = static_cast<int16_t>(npc->GetX());
		sp.z_pos     = static_cast<int16_t>(npc->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(npc->GetID());
		sp.body_type = static_cast<int16_t>(npc->GetBodyType());
		// Pets already in the zone when this client zones in.
		sp.pet_owner_id = WireOwnerIdForSession(s, npc->GetOwnerID());
		sp.cur_hp    = 100;
		sp.race      = static_cast<int8_t>(npc->GetRace());
		// Playerbots appear as player characters (blue nameplate, client behaviour)
		sp.NPC       = (npc->GetNPCTypeID() == static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId))) ? 0 : 1;
		// Cosmetic guild tag for Playerbots: mirrors NPC::FillSpawnStruct.
		// GetPlayerBotGuildId returns 0xFFFFFFFF when the bot rolls "no guild",
		// which truncates to 0xFFFF — the Trilogy "no guild" sentinel.
		if (sp.NPC == 0 && RuleB(PlayerBots, PlayerBotsCanBeGuilded)) {
			sp.GuildID = static_cast<uint16_t>(database.GetPlayerBotGuildId());
		} else {
			sp.GuildID = static_cast<uint16_t>(0xFFFF);
		}
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
					sp.equipcolors[mi] = static_cast<int32_t>(
						Trilogy::NormalizeTintColor(npc->GetEquipmentColor(static_cast<uint8_t>(mi))));
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
		strncpy(sp.name,    TrilogyWireName(npc),  sizeof(sp.name) - 1);
		strncpy(sp.Surname, npc->GetLastName(),    sizeof(sp.Surname) - 1);

		if (sent < 5) {
			LogInfo("[TrilogyZone] NPC[{}] name='{}' id={} race={} size={:.1f} "
			        "x={} y={} z={} body={} class={} level={}",
			        sent, npc->GetCleanName(), npc->GetID(), npc->GetRace(),
			        npc->GetSize(), sp.x_pos, sp.y_pos, sp.z_pos,
			        sp.body_type, sp.class_, sp.level);
		}

		// Diag: FV Maiden. Log inclusion in the bulk with wire-encoded state
		// so we can compare to the individual 4921 path and to the boat's
		// actual server-side position at zone-in time. See Zone:TrilogyBoatDiag.
		if (RuleB(Zone, TrilogyBoatDiag) && npc->GetNPCTypeID() == 84250) {
			LogInfo("[BoatDiag] SendZoneSpawns 6121 Maiden 84250: id={} "
			        "server_pos=({:.1f},{:.1f},{:.1f}) sp.pos=({},{},{}) sp.h={} "
			        "sp.walkspeed={:.2f} sp.runspeed={:.2f} moving={} grid={} "
			        "bulk_idx={} char=[{}]",
			        npc->GetID(),
			        npc->GetX(), npc->GetY(), npc->GetZ(),
			        sp.x_pos, sp.y_pos, sp.z_pos,
			        static_cast<int>(sp.heading),
			        sp.walkspeed, sp.runspeed,
			        npc->IsMoving() ? 1 : 0,
			        npc->GetGrid(),
			        sent, s.char_name);
		}

		// Seed v29c-client-known-material model from the spawn struct.
		if (s.trilogy_client) s.trilogy_client->SeedKnownMaterials(
			static_cast<uint16_t>(sp.spawn_id), sp.equipment);
		// Record for ghost-spawn reconciliation in SendMobHeartbeat, and seed
		// last_broadcast so drift-refresh can catch never-in-cull mobs.
		NoteKnownSpawnAt(SessionKey(addr, port), static_cast<uint16_t>(sp.spawn_id),
		                 sp.x_pos, sp.y_pos, sp.z_pos, sp.heading);

		append_entry(ns);
	}

	// Include Bots (the Bot subsystem — owner-summoned PC-like companions) so
	// that Trilogy players see them as group-eligible PCs (NPC=0, blue
	// nameplate).  Bots live in entity_list.bot_list, NOT npc_map, so the NPC
	// loop above skipped them entirely.
	for (Bot* bot : bot_list) {
		if (!bot) continue;

		Trilogy::structs::NewSpawn_Struct ns{};
		memset(&ns, 0, sizeof(ns));
		Trilogy::structs::Spawn_Struct& sp = ns.spawn;

		sp.size      = bot->GetSize();
		if (sp.size <= 0.0f) sp.size = 6.0f;
		// Bots are player-like (NPC=0 nameplate). Match the player baseline.
		sp.walkspeed = kTrilogyPlayerWalkSpeed;
		sp.runspeed  = kTrilogyPlayerRunSpeed;
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(bot->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(bot->GetY());
		sp.x_pos     = static_cast<int16_t>(bot->GetX());
		sp.z_pos     = static_cast<int16_t>(bot->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(bot->GetID());
		sp.body_type = static_cast<int16_t>(bot->GetBodyType());
		sp.pet_owner_id = WireOwnerIdForSession(s, bot->GetOwnerID());
		sp.cur_hp    = static_cast<int16_t>(bot->GetHPRatio());
		// Bot::IsInAGuild() treats both _guildId==0 (newly-created bot) and
		// _guildId==GUILD_NONE as "no guild", but only GUILD_NONE (0xFFFFFFFF)
		// truncates to the uint16 0xFFFF sentinel — a raw 0 cast becomes guild
		// slot 0 and v29c renders the empty entry as "<Unknown Guild>".
		sp.GuildID   = bot->IsInAGuild()
		                   ? static_cast<uint16_t>(bot->GuildID())
		                   : static_cast<uint16_t>(0xFFFF);
		sp.race      = static_cast<int8_t>(bot->GetRace());
		sp.NPC       = 0; // player nameplate so /invite works on the client
		sp.class_    = static_cast<int8_t>(bot->GetClass());
		sp.gender    = static_cast<int8_t>(bot->GetGender());
		sp.level     = static_cast<int8_t>(bot->GetLevel());
		sp.anim_type         = 0x64; // standing
		sp.npc_armor_graphic = static_cast<int8_t>(0xFF); // PC equipment mode
		sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
		sp.guildrank         = Trilogy::structs::TranslateGuildRankToTrilogy(
		    static_cast<uint8_t>(bot->GuildRank()), bot->IsInAGuild());
		sp.light = static_cast<int8_t>(bot->GetEquipmentLightType());
		strncpy(sp.name,    TrilogyWireName(bot), sizeof(sp.name) - 1);
		strncpy(sp.Surname, bot->GetLastName(),  sizeof(sp.Surname) - 1);
		// Face / hair — unknown163[0..6] mirrors EQClassic offsets 207–213
		sp.unknown163[0] = static_cast<int8_t>(bot->GetHairColor());
		sp.unknown163[1] = static_cast<int8_t>(bot->GetBeardColor());
		sp.unknown163[2] = static_cast<int8_t>(bot->GetEyeColor1());
		sp.unknown163[3] = static_cast<int8_t>(bot->GetEyeColor2());
		sp.unknown163[4] = static_cast<int8_t>(bot->GetHairStyle());
		sp.unknown163[6] = static_cast<int8_t>(bot->GetLuclinFace());
		// Equipment textures and armor tints
		for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
			sp.equipment[mi]   = static_cast<int8_t>(bot->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
			sp.equipcolors[mi] = static_cast<int32_t>(
				Trilogy::NormalizeTintColor(bot->GetEquipmentColor(static_cast<uint8_t>(mi))));
		}
		sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(bot->GetEquipmentMaterial(EQ::textures::weaponPrimary));
		sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(bot->GetEquipmentMaterial(EQ::textures::weaponSecondary));

		// Seed v29c-client-known-material model from the spawn struct.
		if (s.trilogy_client) s.trilogy_client->SeedKnownMaterials(
			static_cast<uint16_t>(sp.spawn_id), sp.equipment);
		// Record for ghost-spawn reconciliation in SendMobHeartbeat, and seed
		// last_broadcast so drift-refresh can catch never-in-cull mobs.
		NoteKnownSpawnAt(SessionKey(addr, port), static_cast<uint16_t>(sp.spawn_id),
		                 sp.x_pos, sp.y_pos, sp.z_pos, sp.heading);

		append_entry(ns);
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
		sp.walkspeed = kTrilogyPlayerWalkSpeed;
		sp.runspeed  = kTrilogyPlayerRunSpeed;
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(c->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(c->GetY());
		sp.x_pos     = static_cast<int16_t>(c->GetX());
		sp.z_pos     = static_cast<int16_t>(c->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(c->GetID());
		sp.body_type = static_cast<int16_t>(c->GetBodyType());
		sp.pet_owner_id = WireOwnerIdForSession(s, c->GetOwnerID());
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
		// LD flag (offset 085) so a client zoning in DURING someone's grace
		// window sees the marker too, not just the observers who were present
		// when the SpawnAppearance went out.  EQClassic stamps it the same way
		// in MakeSpawnUpdate (LS/zone/mob.cpp:572).
		sp.LD = (c->IsTrilogyClient() &&
		         static_cast<TrilogyClient*>(c)->IsLinkdead()) ? 1 : 0;
		sp.guildrank = Trilogy::structs::TranslateGuildRankToTrilogy(
		    static_cast<uint8_t>(c->GuildRank()), c->IsInAGuild());
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
			sp.equipcolors[mi] = static_cast<int32_t>(
				Trilogy::NormalizeTintColor(c->GetEquipmentColor(static_cast<uint8_t>(mi))));
		}

		LogInfo("[TrilogyZone] Player[{}] name='{}' id={} race={} x={} y={} z={}",
		        sent, c->GetCleanName(), c->GetID(), c->GetRace(),
		        sp.x_pos, sp.y_pos, sp.z_pos);

		// Seed v29c-client-known-material model from the spawn struct.
		if (s.trilogy_client) s.trilogy_client->SeedKnownMaterials(
			static_cast<uint16_t>(sp.spawn_id), sp.equipment);
		// Record for ghost-spawn reconciliation in SendMobHeartbeat, and seed
		// last_broadcast so drift-refresh can catch never-in-cull mobs.
		NoteKnownSpawnAt(SessionKey(addr, port), static_cast<uint16_t>(sp.spawn_id),
		                 sp.x_pos, sp.y_pos, sp.z_pos, sp.heading);

		append_entry(ns);
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
				sp.equipcolors[mi] = static_cast<int32_t>(
					Trilogy::NormalizeTintColor(corpse->GetEquipmentColor(static_cast<uint8_t>(mi))));
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

		// Seed v29c-client-known-material model from the spawn struct.
		if (s.trilogy_client) s.trilogy_client->SeedKnownMaterials(
			static_cast<uint16_t>(sp.spawn_id), sp.equipment);
		// Record for ghost-spawn reconciliation in SendMobHeartbeat, and seed
		// last_broadcast so drift-refresh can catch never-in-cull mobs.
		NoteKnownSpawnAt(SessionKey(addr, port), static_cast<uint16_t>(sp.spawn_id),
		                 sp.x_pos, sp.y_pos, sp.z_pos, sp.heading);

		append_entry(ns);
	}

	// Flush the trailing chunk (if any).  Chunks that hit chunk_cap during
	// append_entry have already been emitted; this handles the remainder.
	flush_chunk();

	LogInfo("[TrilogyZone] SendZoneSpawns: {} entries total across {} OP_ZoneSpawns packet(s) ({})",
	        sent, chunk_packets, split_enabled ? "split@100" : "single");
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
	sp.walkspeed = kTrilogyPlayerWalkSpeed;
	sp.runspeed  = kTrilogyPlayerRunSpeed;
	sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(c->GetHeading() / 2.0f));
	sp.y_pos     = static_cast<int16_t>(c->GetY());
	sp.x_pos     = static_cast<int16_t>(c->GetX());
	sp.z_pos     = static_cast<int16_t>(c->GetZ() * 10.0f);
	sp.spawn_id  = static_cast<int16_t>(c->GetID());
	sp.body_type = static_cast<int16_t>(c->GetBodyType());
	sp.pet_owner_id = WireOwnerIdForSessionKey(session_key, c->GetOwnerID());
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
	// LD flag (offset 085) — see the SendZoneSpawns copy above.
	sp.LD = (c->IsTrilogyClient() &&
	         static_cast<TrilogyClient*>(c)->IsLinkdead()) ? 1 : 0;
	sp.guildrank = Trilogy::structs::TranslateGuildRankToTrilogy(
	    static_cast<uint8_t>(c->GuildRank()), c->IsInAGuild());
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
		sp.equipcolors[mi] = static_cast<int32_t>(
			Trilogy::NormalizeTintColor(c->GetEquipmentColor(static_cast<uint8_t>(mi))));
	}

	// Seed v29c-client-known-material model from the spawn struct.
	if (auto sit = m_sessions.find(session_key); sit != m_sessions.end() && sit->second.trilogy_client) {
		sit->second.trilogy_client->SeedKnownMaterials(
			static_cast<uint16_t>(sp.spawn_id), sp.equipment);
		// Record for ghost-spawn reconciliation in SendMobHeartbeat, and seed
		// last_broadcast so drift-refresh can catch never-in-cull mobs.
		NoteKnownSpawnAt(session_key, static_cast<uint16_t>(sp.spawn_id),
		                 sp.x_pos, sp.y_pos, sp.z_pos, sp.heading);
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

	// Follow-up Illusion, matching what every other player-race class gets
	// after its spawn (NPCs and bots in the post-D820 loop, NPCs again in
	// TrilogyClient::HandleNewSpawn).  A single-entry 0x6121 does appear to
	// honour the struct's face byte, so this is belt-and-braces here rather
	// than the fix itself -- but it costs one 72-byte packet per player spawn
	// and removes the last path where a face depends on which delivery route
	// the spawn happened to take.  Safe to send immediately: unlike the bulk
	// zone-in case, the entity is already registered client-side by the time a
	// single spawn lands, which is the condition the post-D820 deferral exists
	// to satisfy.
	if (IsPlayerRace(c->GetRace())) {
		uint8_t il_buf[72];
		FillIllusionBuf(il_buf, c->GetCleanName(),
		    static_cast<int16_t>(c->GetRace()),
		    static_cast<int16_t>(c->GetGender()),
		    static_cast<int16_t>(-1),   // keep current texture/mode
		    static_cast<int16_t>(-1),   // keep current helm
		    static_cast<int16_t>(c->GetLuclinFace()));
		SendToSession(session_key, 0x9120, il_buf, 72);
		LogInfo("[TrilogyFace] mid-session illusion | target=[{}] race={} gender={} face={}",
		        c->GetCleanName(), c->GetRace(), c->GetGender(),
		        static_cast<int>(c->GetLuclinFace()));
	}
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
	// PC-nameplate entities (Bots + Playerbots) get the player speed baseline
	// so they match a real player's local-render speed in the v29c view.
	sp.walkspeed = kTrilogyPlayerWalkSpeed;
	sp.runspeed  = kTrilogyPlayerRunSpeed;
	sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(npc->GetHeading() / 2.0f));
	sp.y_pos     = static_cast<int16_t>(npc->GetY());
	sp.x_pos     = static_cast<int16_t>(npc->GetX());
	sp.z_pos     = static_cast<int16_t>(npc->GetZ() * 10.0f);
	sp.spawn_id  = static_cast<int16_t>(npc->GetID());
	sp.body_type = static_cast<int16_t>(npc->GetBodyType());
	sp.pet_owner_id = WireOwnerIdForSessionKey(session_key, npc->GetOwnerID());
	sp.cur_hp    = static_cast<int16_t>(npc->GetHPRatio());
	// Guild tag.  This helper is reused for both Playerbot NPCs and Bot
	// subsystem entities (Bot inherits from NPC).  Real Bots carry a stored
	// _guildId from botdb.LoadBotGuild; Playerbot NPCs get a random cosmetic
	// tag from the PlayerBotsCanBeGuilded rule.  Mirrors the per-entity-kind
	// split already used by SendZoneSpawns.
	if (npc->IsBot()) {
		// IsInAGuild() catches both _guildId==0 and _guildId==GUILD_NONE; only
		// the latter truncates to the uint16 0xFFFF sentinel — a raw 0 cast
		// hits guild slot 0 and v29c shows "<Unknown Guild>" above the bot.
		sp.GuildID   = npc->CastToBot()->IsInAGuild()
		                   ? static_cast<uint16_t>(npc->CastToBot()->GuildID())
		                   : static_cast<uint16_t>(0xFFFF);
		sp.guildrank = Trilogy::structs::TranslateGuildRankToTrilogy(
		    static_cast<uint8_t>(npc->CastToBot()->GuildRank()),
		    npc->CastToBot()->IsInAGuild());
	} else {
		if (RuleB(PlayerBots, PlayerBotsCanBeGuilded)) {
			sp.GuildID = static_cast<uint16_t>(database.GetPlayerBotGuildId());
		} else {
			sp.GuildID = static_cast<uint16_t>(0xFFFF);
		}
		sp.guildrank = static_cast<int8_t>(0xFF);
	}
	sp.race      = static_cast<int8_t>(npc->GetRace());
	sp.NPC       = 0; // player nameplate
	sp.class_    = static_cast<int8_t>(npc->GetClass());
	sp.gender    = static_cast<int8_t>(npc->GetGender());
	sp.level     = static_cast<int8_t>(npc->GetLevel());
	sp.anim_type         = 0x64; // standing
	sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
	sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
	sp.light = static_cast<int8_t>(npc->GetEquipmentLightType());
	strncpy(sp.name,    TrilogyWireName(npc),  sizeof(sp.name) - 1);
	strncpy(sp.Surname, npc->GetLastName(),    sizeof(sp.Surname) - 1);
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
		sp.equipcolors[mi] = static_cast<int32_t>(
			Trilogy::NormalizeTintColor(npc->GetEquipmentColor(static_cast<uint8_t>(mi))));
	}
	sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponPrimary));
	sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponSecondary));

	// Seed v29c-client-known-material model from the spawn struct.
	if (auto sit = m_sessions.find(session_key); sit != m_sessions.end() && sit->second.trilogy_client) {
		sit->second.trilogy_client->SeedKnownMaterials(
			static_cast<uint16_t>(sp.spawn_id), sp.equipment);
		// Record for ghost-spawn reconciliation in SendMobHeartbeat, and seed
		// last_broadcast so drift-refresh can catch never-in-cull mobs.
		NoteKnownSpawnAt(session_key, static_cast<uint16_t>(sp.spawn_id),
		                 sp.x_pos, sp.y_pos, sp.z_pos, sp.heading);
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

	// Follow up with an Illusion packet (0x9120, 72-byte Zone format) to set
	// face — Spawn_Struct does not carry face for NPCs in Trilogy.
	// texture/helm use 0xFFFF (-1), EQClassic's "keep current" sentinel, so
	// the Illusion does not switch the client to a flat body-texture mode and
	// hide the Playerbot's equipped armor.
	uint8_t il_buf[72];
	FillIllusionBuf(il_buf, TrilogyWireName(npc),
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
			sp.equipcolors[mi] = static_cast<int32_t>(
				Trilogy::NormalizeTintColor(corpse->GetEquipmentColor(static_cast<uint8_t>(mi))));
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

	// Seed v29c-client-known-material model from the spawn struct.
	if (auto sit = m_sessions.find(session_key); sit != m_sessions.end() && sit->second.trilogy_client) {
		sit->second.trilogy_client->SeedKnownMaterials(
			static_cast<uint16_t>(sp.spawn_id), sp.equipment);
		// Record for ghost-spawn reconciliation in SendMobHeartbeat, and seed
		// last_broadcast so drift-refresh can catch never-in-cull mobs.
		NoteKnownSpawnAt(session_key, static_cast<uint16_t>(sp.spawn_id),
		                 sp.x_pos, sp.y_pos, sp.z_pos, sp.heading);
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

	// ── Linkdead ─────────────────────────────────────────────────────────
	// EQClassic goes linkdead off a dead socket -- `!eqnc->CheckActive()` at
	// LS/zone/client_process.cpp:6769 -- then keeps the body for
	// CLIENT_LD_TIMEOUT (30 s, client.h:39) under server AI control, with
	// CLIENT_LINKDEAD still counting as in-zone (client.h:96) so it can be
	// aggroed and killed.  We have no socket to test: EQNetwork sessions are
	// UDP and TrilogyStream reports ESTABLISHED unconditionally.  Silence is
	// the equivalent signal, and it is a reliable one here because the
	// A120 -> 0x4121 heartbeat chain means a live client stamps last_pkt about
	// every 2 s no matter what the player is doing.  kLinkdeadQuietSeconds is
	// set well clear of that cadence but far below the 300 s reap above, which
	// stays as the backstop for anything this misses.
	//
	// A clean camp-out is NOT this path: it tears the session down through the
	// client-driven camp flow before silence ever accumulates, which is the
	// EQClassic split too -- a deliberate quit camps, a dropped link goes LD.
	static constexpr std::time_t kLinkdeadQuietSeconds = 15;

	const uint64_t now_ms_ld = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	std::vector<uint64_t> ld_expired;
	for (auto& kv : m_sessions) {
		Session& cs = kv.second;
		if (cs.state != CONNECTED || !cs.trilogy_client) continue;

		if (cs.linkdead_since_ms == 0) {
			// Silence backstop: a link that dies without managing even a CLOSE
			// (power cut, NAT drop).  The CLOSE handler covers the common case.
			if (now - cs.last_pkt <= kLinkdeadQuietSeconds) continue;
			EnterLinkdead(cs, now_ms_ld);
			continue;
		}

		// Already linkdead.  A genuinely NEW packet means the client came back
		// -- clear the flag and tell observers rather than reaping a live
		// player.  Compared against the stamp taken at entry, not against
		// wall-clock silence: the CLOSE that usually triggers the hold updates
		// last_pkt itself, so a silence test passes instantly and cancels the
		// hold on the next tick.
		if (cs.last_pkt > cs.linkdead_entry_pkt) {
			cs.linkdead_since_ms  = 0;
			cs.linkdead_entry_pkt = 0;
			cs.trilogy_client->SetLinkdead(false);

			auto outapp = new EQApplicationPacket(
			    OP_SpawnAppearance, sizeof(::SpawnAppearance_Struct));
			auto* sa = reinterpret_cast<::SpawnAppearance_Struct*>(outapp->pBuffer);
			sa->spawn_id  = cs.trilogy_client->GetID();
			sa->type      = AppearanceType::Linkdead;
			sa->parameter = 0;
			entity_list.QueueClients(cs.trilogy_client, outapp, true);
			safe_delete(outapp);

			cs.trilogy_client->AI_Stop();
			LogInfo("[TrilogyZone] Linkdead cleared (client resumed): char=[{}]", cs.char_name);
			continue;
		}

		if (now_ms_ld - cs.linkdead_since_ms >= kLinkdeadHoldMs)
			ld_expired.push_back(kv.first);
	}
	for (uint64_t key : ld_expired) {
		LogInfo("[TrilogyZone] Linkdead hold expired, removing session");
		RemoveSession(key);
	}

	uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	for (auto& kv : m_sessions) {
		Session& s = kv.second;
		if (s.state != CONNECTED) continue;
		// Linkdead hold: the body stays in the world but the far end is gone,
		// so skip every per-session outbound (heartbeat, cooldown pushes,
		// queued text).  The LD sweep above owns this session until it expires.
		if (s.linkdead_since_ms != 0) continue;

		// Stale TrilogyClient* guard.  EQEmu's entity_list can drop the Client
		// object out from under us (Client::Process() returns false → entity.cpp
		// "Dropping client" path → ~Client) without notifying TrilogyZoneServer,
		// leaving s.trilogy_client pointing at freed memory.  Tick later calls
		// methods on it (CheckSpellGemCooldowns, DrainPendingText, …) and reads
		// garbage member values — observed crash: SendToSession AV via stale
		// m_tzs in CheckSpellGemCooldowns, ~45 s after a Dropping-client log.
		// Validate via entity_list lookup using the cached EQEmu entity_id (NOT
		// player_spawn_id — that's a wire ID derived from char_id, and a lookup
		// by it returns nullptr, which would cause this guard to wrongly null
		// the pointer on every Tick and silently break input dispatch for
		// Consider, CombatAbility, Hail, etc. in OnDatagram).
		if (s.trilogy_client && s.eqemu_entity_id != 0) {
			Client* live = entity_list.GetClientByID(s.eqemu_entity_id);
			if (live != static_cast<Client*>(s.trilogy_client)) {
				s.trilogy_client  = nullptr;
				s.eqemu_entity_id = 0;
			}
		}

		// BWDiag: every 5 s, emit per-session outbound bandwidth roll.  Gives
		// us hard numbers to disambiguate cumulative bandwidth pressure from
		// other freeze hypotheses.  v29c was sized for ~56k-modem-era inbound
		// (~3-5 KB/s sustained); anything over that is over budget.
		if (s.bw_window_start_ms == 0) {
			s.bw_window_start_ms = now_ms;
		} else if (now_ms - s.bw_window_start_ms >= 5000) {
			uint64_t elapsed_ms = now_ms - s.bw_window_start_ms;
			double   secs      = static_cast<double>(elapsed_ms) / 1000.0;
			double   kbps      = (secs > 0.0) ? (static_cast<double>(s.bw_bytes_sent) / 1024.0) / secs : 0.0;
			double   pps       = (secs > 0.0) ? (static_cast<double>(s.bw_packets_sent)) / secs : 0.0;
			LogInfo("[TrilogyDiag] BW char='{}' window={:.1f}s tx_bytes={} tx_packets={} kbps={:.2f} pps={:.1f}",
			        s.char_name, secs,
			        s.bw_bytes_sent, s.bw_packets_sent, kbps, pps);
			s.bw_window_start_ms = now_ms;
			s.bw_bytes_sent      = 0;
			s.bw_packets_sent    = 0;
		}

		// Camp completion is now driven by the client via ZN_OP_DeleteSpawn
		// (0x5021) at T+30s from OP_Camp — the authoritative Verant-era
		// "camp finished" signal.  Handling it there instead of on a
		// server-side timer avoids two nasty bugs:
		//   1. When bots tanked all incoming damage the server used to see
		//      GetAggroCount()>0 and silently abort camping, but the client
		//      (which never took damage and never saw a local interrupt)
		//      kept its own 30s countdown running, then froze at T+30s
		//      waiting for a reply to its 0x5021/0x5521 handshake.
		//   2. When the player was legitimately interrupted (took damage,
		//      client shows "You have been interrupted while camping."), a
		//      server-side timer would still fire at T+29s and force-
		//      disconnect a now-active player mid-fight.
		// The client is the only actor that knows if the countdown was
		// interrupted, so let it drive.  If the client crashes mid-camp the
		// existing 300 s CONNECTED session timeout (above) reaps the state.

		// Skip timed broadcasts while the client is in zone-out transition.
		// Stamina/TimeOfDay packets arriving during EQNetwork's CLOSE handshake
		// can corrupt the connection-table cleanup, leaving a freed-pointer sentinel
		// (0xff000000) instead of NULL — which causes the 0x004c7752 crash on the
		// next zone-back to this zone.
		if (s.trilogy_client && s.trilogy_client->IsZoning()) continue;

		// Per-NPC spawn-liveness refresh — matches EQClassic's per-NPC 5 s
		// spawnUpdate_timer in NPC::Process (Zone/Source/npc.cpp:683-691,
		// constructor-staggered 125-999 ms at npc.cpp:144-146). Yeahlight's
		// comment at npc.cpp:682 is explicit: "This fixes the client's
		// inability to target via F8 key and also restores each mob's random
		// sound effects (flapping wings, growls, etc)" — the exact symptoms
		// we hit when this call was disabled (mobs visible-but-not-attackable,
		// "futile to consider the dead", loot/quest interactions broken on
		// stationary NPCs).
		//
		// The prior disable rationale conflated two unrelated EQClassic code
		// paths: client_process.cpp:528 ("no need to refresh player position")
		// is about PLAYER→other-clients refresh; npc.cpp:683-691 is the
		// NPC→player liveness signal and was never optional.
		//
		// SendMobHeartbeat is self-throttled (per-spawn last.sent_ms +
		// STALENESS_REFRESH_MS = 5000), so per-Tick calls only emit packets
		// for NPCs that are actually stale. Batched 25-per-A120 packet keeps
		// the wire cost negligible (~1.8 pps for 217 stationary Freport NPCs).
		SendMobHeartbeat(s.source_addr, s.source_port, s);

		// Spell gem cooldown expiry: un-grey gems whose recast timers have elapsed.
		if (s.trilogy_client) {
			s.trilogy_client->CheckSpellGemCooldowns();
		}

		// Paced OP_SpecialMesg drain: see QueueTextPacket in trilogy_client.
		// v29c freezes on tight 0x8021 bursts (observed ^spells with 9 lines
		// in <50 ms); we cap outbound chat at 20 lines/s.
		if (s.trilogy_client) {
			s.trilogy_client->DrainPendingText();
		}

		// Drain the per-Tick A120 batch for moving mobs. HandleClientUpdate
		// accumulates accepted-and-throttled MobUpdates into a per-session
		// buffer; here we emit them as 25-mob bulk A120 packets. Cuts
		// moving-mob ARQ overhead ~20× — the architectural follow-up to
		// the stationary-mob heartbeat re-enable, both aimed at pushing
		// past the v29c ~4870-ARQ session ceiling.
		if (s.trilogy_client) {
			s.trilogy_client->FlushPendingMobUpdates();
		}

		// SE_Fear tick hook — currently only resets self-push transition
		// state when IsFeared() flips off (so the next fear cast re-logs
		// its first push).  Actual fear position pushes are event-driven
		// via HandleClientUpdate's self-branch — MoveToCommand fires
		// SendCommandToClients on each new fear leg / speed-change /
		// 5s heartbeat and the self-echo now routes to A120.  EQClassic
		// parity: one A120 per leg + client-side heading × anim_type
		// extrapolation between packets.  Previous 100ms tick heartbeat
		// caused visible jitter by over-correcting the extrapolation.
		if (s.trilogy_client) {
			s.trilogy_client->MaybeSendFearHeartbeat();
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

		// Refresh stamina every 5s.  fatigue stays 0 so client endurance never
		// depletes; food/water mirror the real PlayerProfile so v29c can drive
		// its own auto-consume loop (sends ZN_OP_ConsumeFoodDrink when its
		// internal threshold is crossed).  Unreliable: see the zone-in send for
		// rationale.  Loss is harmless — next tick re-sends the latest values.
		if (s.trilogy_client && now_ms - s.last_stamina_ms >= 5000) {
			s.last_stamina_ms = now_ms;
			const auto& pp = s.trilogy_client->GetPP();
			Trilogy::structs::Stamina_Struct sta{};
			sta.food    = static_cast<int16_t>(std::min(std::max(pp.hunger_level, 0), 6000));
			sta.water   = static_cast<int16_t>(std::min(std::max(pp.thirst_level, 0), 6000));
			sta.fatigue = 0;
			SendApp(s.source_addr, s.source_port, s, ZN_OP_Stamina,
			        reinterpret_cast<const uint8_t*>(&sta), sizeof(sta),
			        /*ack_req=*/false);
		}

		// Re-sync EQ clock every 180s (1 EQ hour), matching the world server's
		// periodic broadcast.  This keeps the client's sky/lighting updated as
		// EQ time advances.  When the zone is time-locked (airplane perma-dusk),
		// the client's own local ticker would drift the sky forward a full EQ
		// hour between 180s heartbeats before snapping back — re-emit every 10s
		// so the drift stays inside ~3 EQ minutes and the sunset gradient holds
		// visually steady.
		const uint64_t tod_interval =
			(zone && IsTrilogyTimeLocked(zone->GetShortName())) ? 10000 : 180000;
		if (now_ms - s.last_time_of_day_ms >= tod_interval) {
			s.last_time_of_day_ms = now_ms;
			SendTimeOfDay(s.source_addr, s.source_port, s);
		}
	}

	// ──────────────────────────────────────────────────────────────────────
	// Per-packet exponential-backoff ARQ retransmit + age-based linkdead drop
	//
	// Replaces the prior EQClassic-port that retried every queued packet on a
	// session-wide 1 s timer with a hard 15-retry cap.  That design caused
	// "drop avalanches" — when the HEAD packet stalled and the timer fired
	// 15 times in 15 s, every TAIL packet pushed during the stall got retried
	// in lockstep and hit count=15 simultaneously, dropping 3-8 ARQs at the
	// same instant (visible in resend-cap log clusters at session end).
	//
	// Each PendingArq now carries:
	//   first_sent_ms — for the HEAD-only age check (linkdead drop)
	//   next_retry_ms — its own per-packet retry timer
	//   backoff_ms    — doubles after each retransmit, capped at kMaxBackoffMs
	//
	// Drop policy: when the OLDEST unacked packet has been pending > 30 s,
	// the session is treated as linkdead and removed.  Tail packets get their
	// own independent first_sent_ms so they can't be avalanched by a head
	// stall.  The 30 s tolerance matches Verant's classic linkdead behaviour
	// (sustained unresponsiveness, not a fast-retry count, is what signals a
	// truly dead session).
	//
	// Retry schedule per packet: 500 ms → 1 s → 2 s → 4 s → 8 s → 16 s (cap).
	// Five retries fit inside the 30 s window before the linkdead check fires.
	// Cumulative ACK pop in OnDatagram (line 738+) is untouched and still
	// drains acked entries; this loop only handles the loss-recovery and
	// drop branches.  Wire-format SEQ patch + CRC recompute is unchanged.
	{
		constexpr uint64_t kLinkdeadMs       = 30000;
		constexpr uint64_t kMaxBackoffMs     = 16000;
		// Per-session, per-Tick retransmit cap.  When N packets all hit
		// their next_retry_ms simultaneously (head stalled for a while),
		// firing all N back-to-back in one Tick produces a UDP burst that
		// v29c's modem-era recv buffer can silently overflow.  Cap at 3 per
		// Tick — the rest defer to the next Tick (~30 ms later) so the
		// client has time to drain its socket between waves.  Their
		// next_retry_ms is unchanged, so they're still due immediately on
		// the next Tick; we just spread the wave over a few cycles instead
		// of bursting it all at once.  See [[project-trilogy-resend-
		// explosion]] 2026-06-23 retransmit-burst investigation.
		constexpr uint32_t kMaxRetriesPerTick = 3;
		std::vector<uint64_t> to_drop;

		for (auto& kv : m_sessions) {
			Session& s = kv.second;
			if (s.state != CONNECTED) continue;
			if (s.resend_queue.empty()) continue;

			// HEAD-only linkdead check.  Queue is FIFO with packets pushed
			// in ARQ order, so the front is always the oldest unacked.
			// If the head hasn't aged out, no entry has — early exit.
			//
			// Underflow guard: this Tick captured now_ms once at the top,
			// but earlier Tick callbacks (SendMobHeartbeat, FlushPending-
			// MobUpdates, money reconciliation, Stamina refresh, etc.)
			// may have called SendApp, which re-reads the clock to set
			// first_sent_ms.  Net effect: head.first_sent_ms can be a
			// few ms AHEAD of Tick's now_ms.  Without the guard the
			// unsigned subtraction wraps to ~2^64 and trivially exceeds
			// kLinkdeadMs — dropping a brand-new packet instantly.
			// Test log 1320 (2026-06-23): F220 pushed at the end of Tick,
			// reported "stuck 18446744073709551597ms" the same instant.
			{
				const auto& head = s.resend_queue.front();
				if (now_ms >= head.first_sent_ms) {
					const uint64_t head_age_ms = now_ms - head.first_sent_ms;
					if (head_age_ms > kLinkdeadMs) {
						LogInfo("[TrilogyZone] linkdead: head ARQ={:04X} opcode={:04X} "
						        "stuck {}ms (send_count={}) qsize={} for char [{}] — dropping",
						        head.arq, head.opcode, head_age_ms, head.send_count,
						        s.resend_queue.size(), s.char_name);
						to_drop.push_back(kv.first);
						continue;
					}
				}
				// else: head was pushed AFTER Tick's now_ms snapshot —
				// it can't possibly be stuck.  Fall through to the per-
				// packet retry loop (which uses safe future-comparison
				// against next_retry_ms, no subtraction needed).
			}

			// Per-packet retransmit — walk the queue, resend only entries
			// whose individual next_retry_ms has fired.  Apply exponential
			// backoff to the resent entry's next slot.  Cap at
			// kMaxRetriesPerTick per session per Tick to prevent UDP burst
			// overflow on the v29c client side; deferred packets retain
			// their (already-elapsed) next_retry_ms and fire on the next
			// Tick, spreading the wave over several Tick cycles.
			uint32_t retries_this_tick = 0;
			for (auto& pending : s.resend_queue) {
				if (pending.wire_bytes.size() < 8) continue; // sanity
				if (now_ms < pending.next_retry_ms)  continue;
				if (retries_this_tick >= kMaxRetriesPerTick) break;

				// Patch SEQ at offset 2-3 to a fresh value.  ARSP/ARQ
				// payload preserved as originally sent.
				uint16_t fresh_seq = htons(s.gsq++);
				memcpy(pending.wire_bytes.data() + 2, &fresh_seq, 2);

				// Recompute CRC32 over bytes [0..size-4].
				size_t crc_off = pending.wire_bytes.size() - 4;
				uint32_t crc = htonl(CRC32::Generate(
					pending.wire_bytes.data(), static_cast<uint32_t>(crc_off)));
				memcpy(pending.wire_bytes.data() + crc_off, &crc, 4);

				m_send_fn(s.source_addr, s.source_port,
				          reinterpret_cast<const char*>(pending.wire_bytes.data()),
				          pending.wire_bytes.size());

				s.bw_bytes_sent   += static_cast<uint64_t>(pending.wire_bytes.size());
				s.bw_packets_sent += 1;
				++retries_this_tick;

				pending.last_send_ms = now_ms;
				pending.send_count   = static_cast<uint16_t>(pending.send_count + 1);

				// Exponential backoff: double the interval, cap at kMaxBackoffMs.
				// Drop check above fires before backoff can grow beyond the cap.
				pending.backoff_ms    = std::min<uint64_t>(pending.backoff_ms * 2, kMaxBackoffMs);
				pending.next_retry_ms = now_ms + pending.backoff_ms;
			}
		}

		for (uint64_t key : to_drop) RemoveSession(key);
	}

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
		constexpr int      kMaxUnacked    = 9; // EQClassic-style outbound-pending cap

		for (auto& kv : m_sessions) {
			Session& s = kv.second;
			if (s.outbound_queue.empty()) continue;
			if (s.state != CONNECTED) continue;

			if (now_ms - s.outbound_window_start_ms >= WINDOW_MS) {
				s.outbound_window_count    = 0;
				s.outbound_window_start_ms = now_ms;
			}

			// Two-gate drain: pop while BOTH (1) under per-100ms burst budget,
			// AND (2) under unacked-pending cap.  Either gate full → leave the
			// rest queued for the next Tick.  Unacked drains as client ARSP
			// arrives in OnDatagram (s.acked_arq advances).
			s.draining_outbound = true;
			while (!s.outbound_queue.empty()
			       && s.outbound_window_count < WINDOW_PACKETS
			       && static_cast<int16_t>(s.arq - s.acked_arq) < kMaxUnacked) {
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
	// off the v29c receive buffer. Combat: 200 ms (5 Hz) — a charging mob can close
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
	//
	// 2026-06-21: combat throttle relaxed from 100 → 200 ms after Test 7024
	// proved the freeze trigger is the cumulative A120↔4121 round-trip count.
	// v29c sends a 4121 ARQ for every A120 we send; at 50 visible mobs × 10 Hz
	// throttle we were generating 30+ round-trips/sec sustained, hitting some
	// internal v29c queue ceiling around 5000-6000 trips at ~7 minutes
	// regardless of bandwidth/packet/entity-table mitigations.  Halving the
	// rate doubles the time-to-freeze and gets us out of the danger zone for
	// reasonable session lengths.  Idle Kaladim (10+ min clean) already runs
	// at ~4-8 trips/sec — well under the limit — so 4 Hz idle is unchanged.
	uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	// TEMP TEST 2026-06-27: base throttle at 2000ms.  At 1000ms the int16-
	// truncation lateral wobble returns visibly; at 2000ms motion is smooth
	// but each update lands a visible forward snap (client extrapolation
	// slower than server motion).  Tradeoff settled in favor of 2000ms;
	// fix the residual forward snap by adjusting anim_type encoding so the
	// v29c client extrapolates at the server's actual speed.
	uint32_t throttle_ms = 2000;
	if (s.trilogy_client && s.trilogy_client->GetAggroCount() > 0) {
		throttle_ms = 2000;
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
		if (s.nearby_combat) throttle_ms = 200;
	}

	// Rotation-smoothing throttle: with delta_heading=0 (spin-stun field),
	// the v29c client SNAPS heading on each received update.  EQClassic's
	// equivalent NPC code (npc.cpp:1276) fires a position update every
	// time heading changes by >20 wire units, producing ~12-20 Hz update
	// rate during a rotation.  At our default 4 Hz heartbeat, a typical
	// walking FaceTarget (~220 ms / 90°) yields only 1 heading snap mid-
	// rotation, which renders as "jagged / missed frames".
	//
	// When any visible NPC/Bot is mid-rotation (turning flag set by
	// RotateToCommand at mob_movement_manager.cpp:67), drop the global
	// throttle to kTurningThrottleMs so the rotation gets multiple heading
	// snaps across its duration.  Auto-reverts to 250 ms once the turning
	// mob completes (RotateToCommand sets turning=false at
	// mob_movement_manager.cpp:88) and the scan cache (kTurningScanCacheMs)
	// re-validates with no turning mobs found.
	//
	// Tuning: a walking FaceTarget covers 90° in ~220 ms. At 20 ms throttle
	// that's ~11 updates → ~8° per snap, well below the eye's motion-
	// fusion threshold for rotational motion. Effectively the server's
	// Tick rate (main.cpp's Sleep(1) loop with EQ overhead lands around
	// 50 Hz wall-clock) so we can't go faster without re-architecting
	// the heartbeat to skip the loop iteration.  Cost grows linearly
	// with rate, but the bypass only fires while turning=true on at
	// least one in-range mob — i.e. only during the brief rotation
	// window itself, so steady-state bandwidth is unaffected.
	static constexpr uint32_t kTurningThrottleMs    = 10;
	static constexpr uint64_t kTurningScanCacheMs   = 100;  // scan cost minor; refresh faster than the throttle so turning-end detection is snappy
	if (s.trilogy_client && now_ms - s.last_turning_scan_ms >= kTurningScanCacheMs) {
		s.last_turning_scan_ms = now_ms;
		s.nearby_turning       = false;
		constexpr float TURN_RADIUS_SQ = 600.0f * 600.0f; // match CULL_RADIUS
		for (const auto& kv : entity_list.GetNPCList()) {
			NPC* npc = kv.second;
			if (!npc || !npc->turning) continue;
			float dx = npc->GetX() - s.pos_x;
			float dy = npc->GetY() - s.pos_y;
			if (dx * dx + dy * dy <= TURN_RADIUS_SQ) {
				s.nearby_turning = true;
				break;
			}
		}
		if (!s.nearby_turning) {
			for (Bot* bot : entity_list.GetBotList()) {
				if (!bot || !bot->turning) continue;
				float dx = bot->GetX() - s.pos_x;
				float dy = bot->GetY() - s.pos_y;
				if (dx * dx + dy * dy <= TURN_RADIUS_SQ) {
					s.nearby_turning = true;
					break;
				}
			}
		}
	}
	if (s.nearby_turning && throttle_ms > kTurningThrottleMs)
		throttle_ms = kTurningThrottleMs;

	// Moving-mob smoothing throttle: with kVelocityWireScale=0 (EQClassic-
	// faithful — v29c client receives no inter-entity delta_x/y/z and so
	// cannot extrapolate motion between heartbeats), the rendered position
	// of a moving NPC stays frozen between updates while the server-side
	// position drifts forward.  At the default 250 ms throttle and ~12
	// units/sec server motion that yields a ~3-unit position snap per
	// heartbeat — visibly jagged on diagonal pathing.  Drop the throttle
	// to kMovingThrottleMs when any in-range NPC/Bot is moving so each snap
	// is ~0.6 units, below the visible-jaggedness threshold.  Bypass auto-
	// clears once the scan finds no moving mobs (mob completes a path
	// segment / arrives at waypoint), returning to the 250 ms idle rate.
	//
	// Bandwidth cost: ~20 Hz on the in-range moving subset only.  With
	// typically 5-10 movers visible at ~26 B/mob/update, that's ~2.5-5 KB/s
	// per player — well inside the headroom proven by the long-session BW
	// logs (steady-state <1 kbps idle).  Symmetric in spirit and code to
	// the turning bypass above.
	static constexpr uint32_t kMovingThrottleMs  = 2000;
	static constexpr uint64_t kMovingScanCacheMs = 100;
	if (s.trilogy_client && now_ms - s.last_moving_scan_ms >= kMovingScanCacheMs) {
		s.last_moving_scan_ms = now_ms;
		s.nearby_moving       = false;
		constexpr float MOVE_RADIUS_SQ = 600.0f * 600.0f; // match CULL_RADIUS / turning scan
		for (const auto& kv : entity_list.GetNPCList()) {
			NPC* npc = kv.second;
			if (!npc || !npc->IsMoving()) continue;
			float dx = npc->GetX() - s.pos_x;
			float dy = npc->GetY() - s.pos_y;
			if (dx * dx + dy * dy <= MOVE_RADIUS_SQ) {
				s.nearby_moving = true;
				break;
			}
		}
		if (!s.nearby_moving) {
			for (Bot* bot : entity_list.GetBotList()) {
				if (!bot || !bot->IsMoving()) continue;
				float dx = bot->GetX() - s.pos_x;
				float dy = bot->GetY() - s.pos_y;
				if (dx * dx + dy * dy <= MOVE_RADIUS_SQ) {
					s.nearby_moving = true;
					break;
				}
			}
		}

		// Separate pass for players.  Neither this scan nor the combat and
		// turning scans above ever looked at GetClientList(), so with two
		// players alone in a zone -- no NPC engaged, none turning, none
		// moving -- nothing lowered the throttle and each observed the other
		// at the 2 s baseline.  At run speed that is ~90 units of travel per
		// update, which is the bulk of the "ghosty players" report.
		//
		// Pivoting counts as well as translating.  A player turning on the spot
		// is invisible to every other signal: position does not change so
		// IsMoving() is false, and `turning` is only set by RotateToCommand,
		// which a human turning their own character never goes through.  It
		// rides the same 200 ms bucket rather than the NPC turning path's
		// 10 ms: the wire says a pivoting client reports ~3.2 times a second,
		// so 5 Hz already delivers every heading it sends, and 100 Hz would
		// just spin the loop for nothing.
		s.nearby_moving_player = false;
		for (const auto& kv : entity_list.GetClientList()) {
			Client* other = kv.second;
			if (!other || !other->InZone()) continue;
			if (s.trilogy_client && other == s.trilogy_client) continue;
			const bool active =
				other->IsMoving() ||
				(other->IsTrilogyClient() &&
				 static_cast<TrilogyClient*>(other)->IsRotating(now_ms));
			if (!active) continue;
			float dx = other->GetX() - s.pos_x;
			float dy = other->GetY() - s.pos_y;
			if (dx * dx + dy * dy <= MOVE_RADIUS_SQ) {
				s.nearby_moving_player = true;
				break;
			}
		}
	}
	if (s.nearby_moving && throttle_ms > kMovingThrottleMs)
		throttle_ms = kMovingThrottleMs;

	// A moving player in range gets the combat cadence.  200 ms is not a new
	// number -- it is the rate this heartbeat already sustains for the whole
	// entity set whenever any NPC is engaged nearby, so it is known to sit
	// inside the v29c ARQ budget.  (A120 is sent unreliable, so these do not
	// consume ARQ round-trips at all; see flush_packet.)  Per-entity caps in
	// should_broadcast -- 100 ms inner ring, 500 ms outer -- plus its
	// dirty-flag check still bound what actually goes on the wire, so a zone
	// full of stationary NPCs costs nothing extra while a player runs past.
	static constexpr uint32_t kMovingPlayerThrottleMs = 200;
	if (s.nearby_moving_player && throttle_ms > kMovingPlayerThrottleMs)
		throttle_ms = kMovingPlayerThrottleMs;

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
		// A120 unreliable: original Verant servers sent position updates
		// without ARQ (Agz, EQClassic/Common/Source/EQPacketManager.cpp:412).
		// Loss is harmless (next heartbeat supersedes) and unreliable A120
		// bypasses the v29c client's reliable-stream buffer that drives the
		// ~4870-packet session wall. SendApp's wire format omits dbASQ_low
		// on !ack_req packets to match EQClassic. See
		// [[project-trilogy-unreliable-a120-wire-format]].
		SendApp(addr, port, s, ZN_OP_MobUpdate,
		        pkt, static_cast<uint32_t>(4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)),
		        /*ack_req=*/false);
		n = 0;
	};

	// Only heartbeat NPCs within 600 EQ units of the player.  Sending A120 for
	// every NPC in a large zone (e.g. 334 in ecommons) produces ~56 ARQ packets/sec
	// which overflows the Trilogy v29c client's ARQ queue and causes a disconnect.
	static constexpr float CULL_RADIUS_SQ = 600.0f * 600.0f;

	// Distance tier: within 300 EQ units of the player ("inner ring") we keep
	// the full 100 ms / 10 Hz combat refresh for smooth melee.  Between 300
	// and 600 units ("outer ring") we enforce a 500 ms minimum interval per
	// mob — bot-vs-NPC fights and patrolling guards at that range produce
	// 7-9 kbps of heartbeat amplification, which sustained for 2-3 min has
	// been measured to trigger v29c's cumulative-damage freeze 4-6 min later
	// (Test 2 / 2026-06-21 freportw_*_7000_7108.log).  Cutting outer-ring
	// rate to 2 Hz drops peak BW to ~3-4 kbps without touching what the
	// player is actually engaged with.  Mobs *near* the player are
	// unaffected — by definition combat targets are close.
	static constexpr float    INNER_RING_RADIUS_SQ        = 300.0f * 300.0f;
	static constexpr uint64_t OUTER_RING_MIN_INTERVAL_MS  = 500;
	// Inner-ring min-interval bounds wire cost during nearby-turning
	// fast-cycles where body throttle_ms drops to 10 ms. Without this,
	// inner-ring moving mobs would emit at ~100 Hz for the ~200-500 ms
	// rotation window. 100 ms = 10 Hz per mob, plenty for melee smoothness.
	static constexpr uint64_t INNER_RING_MIN_INTERVAL_MS  = 100;

	// Dirty-flag gate (mirrors EQClassic EntityList::SendPositionUpdates'
	// `GetLastChange() >= cLastUpdate` filter): only stage an update if the
	// wire-encoded position/heading/anim actually changed since the last
	// broadcast to this session, OR the previous broadcast is older than
	// STALENESS_REFRESH_MS.
	//
	// Matches EQClassic's per-NPC `spawnUpdate_timer` cadence: NPC::Process
	// at Zone/Source/npc.cpp:683-691 unconditionally fires SendPosUpdate
	// every 5 s (timer reset at line 686, initial 125-999 ms randomized
	// stagger from npc.cpp:144-146 so 200+ NPCs don't herd onto one Tick).
	// Yeahlight's comment at npc.cpp:682: "This fixes the client's inability
	// to target via F8 key and also restores each mob's random sound effects
	// (flapping wings, growls, etc)" — exactly our staleness symptoms.
	//
	// (Note: the maalanar 2008 comment at client_process.cpp:528 that
	// disabled SendPositionUpdates is about PLAYER→other-clients refresh,
	// NOT NPC→player liveness. Two different paths; conflating them was
	// the 2026-06-21 misread.)
	//
	// Moving mobs are unaffected (dirty flag fires on position change),
	// combat is untouched (per-session 200 ms throttle path above).
	//
	// Caller is expected to have already built `upd` (spawn_id + the encoded
	// int16/int8 wire fields), and to pass `dist_sq` (squared distance from
	// player) so the outer-ring throttle can fire.  Pass `priority=true` to
	// bypass the outer-ring 500 ms rate cap (used for actively-rotating
	// mobs so the user gets smooth rotation on /hail at any visible range).
	// Returns true → include in batch and bump n; false → drop this slot,
	// do not increment n (slot gets overwritten next).
	static constexpr uint64_t STALENESS_REFRESH_MS = 5000;
	auto should_broadcast = [&](Trilogy::structs::SpawnPositionUpdate_Struct* upd,
	                            float dist_sq,
	                            bool  priority = false) -> bool {
		auto& last = s.last_broadcast[static_cast<uint16_t>(upd->spawn_id)];

		// Per-mob min-interval throttle:
		//   Outer ring (>300 u): 500 ms cap — biggest single bandwidth lever,
		//                         most NPCs in the 600 u visible field sit in
		//                         the outer annulus (3× the area of inner).
		//   Inner ring (≤300 u): 100 ms cap — bounds worst case during the
		//                         nearby-turning fast-cycle (body at 10 ms).
		// Turning mobs bypass via priority=true (need every-tick rotation).
		if (!priority) {
			const uint64_t min_interval =
				(dist_sq > INNER_RING_RADIUS_SQ)
				    ? OUTER_RING_MIN_INTERVAL_MS
				    : INNER_RING_MIN_INTERVAL_MS;
			if (now_ms - last.sent_ms < min_interval)
				return false;
		}

		bool stale   = (now_ms - last.sent_ms) >= STALENESS_REFRESH_MS;
		bool changed = (last.x_pos     != upd->x_pos)
		            || (last.y_pos     != upd->y_pos)
		            || (last.z_pos     != upd->z_pos)
		            || (last.heading   != upd->heading)
		            || (last.anim_type != upd->anim_type)
		            || (last.delta_heading != upd->delta_heading);
		if (!stale && !changed) return false;
		last.x_pos     = upd->x_pos;
		last.y_pos     = upd->y_pos;
		last.z_pos     = upd->z_pos;
		last.heading   = upd->heading;
		last.anim_type = upd->anim_type;
		last.delta_heading = upd->delta_heading;
		last.sent_ms   = now_ms;
		return true;
	};

	const uint32_t playerbot_type_id = static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId));

	for (const auto& kv : npc_map) {
		NPC* npc = kv.second;
		if (!npc) continue;

		bool is_playerbot = (npc->GetNPCTypeID() == playerbot_type_id);

		// 2026-06-27 → 2026-08-15: previously we skipped moving-but-not-
		// turning NPCs entirely, on the theory that TrilogyClient::
		// HandleClientUpdate (event-driven from EQEmu's OP_ClientUpdate
		// broadcasts) was the sole source of position for movers.  That
		// theory failed in open zones (commons/lakeofillomen): the
		// [Trilogy desync-pos] detector caught patrolling NPCs going 4-15 s
		// between position updates and rendering at stale coordinates
		// while alive server-side — user-visible symptom "mob just stands
		// there, attacks fail, then eventually poofs".  Root cause: EQEmu's
		// per-NPC broadcast cadence for grid roamers is ~5 s (matching
		// EQClassic's spawnUpdate_timer), and any packet drop in the ARQ
		// pipeline widens the gap.
		//
		// Fix: let moving NPCs flow through should_broadcast just like
		// stationary ones.  The dirty-flag gate suppresses redundant
		// sends when the client already has the current position; the
		// STALENESS_REFRESH_MS forced refresh catches gaps.  The moving-
		// mob branch below (~9078) already encodes proper anim_type and
		// velocity delta from the MovementManager cache, so alternating
		// with event-driven no longer breaks client extrapolation (the
		// obsolete concern that motivated the original skip).
		//
		// Wire cost: at 2 s session heartbeat cadence and dirty-flag
		// suppression, a moving mob costs at most ~0.5 Hz beyond event-
		// driven — bounded by CULL_RADIUS_SQ and outer-ring 500 ms cap.
		// Turning mobs still take the fast (10 ms) path via nearby_turning.
		//
		// Cadence: NO per-tick modulo gate.  The prior `(now_ms/100)%5==0`
		// gate assumed a ~100 ms body cadence, but session throttle_ms is
		// normally 2000 ms.  With body running every 2000 ms and gate
		// checking `now_ms/100 % 5`, alignment is fixed relative to the
		// first body run: if the first run lands off-modulo (e.g.
		// s.last_heartbeat_ms=12345 → 123%5=3), every subsequent 2000 ms
		// later run also lands off-modulo → gate blocks FOREVER, moving
		// mobs never heartbeat.  Diagnosed 2026-08-15 via commons log
		// showing `hb_age_ms=0 cadence_ok=0` on every desync-pos line.
		//
		// Rely on session throttle_ms (2000 ms idle / 200 ms combat / 10 ms
		// during turning) for cadence.  should_broadcast enforces per-mob
		// caps (INNER_RING_MIN_INTERVAL_MS=100 / OUTER_RING_MIN_INTERVAL_MS=500)
		// to bound wire cost during the 10 ms turning fast-cycle.

		float dx = npc->GetX() - s.pos_x;
		float dy = npc->GetY() - s.pos_y;
		float dist_sq = dx * dx + dy * dy;
		if (dist_sq > CULL_RADIUS_SQ) continue;

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
		// delta_heading stays 0 — see ComputeTrilogyDeltaHeading comment for
		// the v29c spin-stun semantics (it's NOT a smoothing hint).

		// delta_x/delta_y stay 0 (EQClassic NPC behaviour); position snaps to
		// server coords each tick without client-side dead-reckoning.
		// anim_type>0 only when moving — stationary mobs send anim_type=0 which
		// plays the idle/stand animation.  The periodic A120 refresh above keeps
		// the staleness timer from firing even with anim_type=0.
		//
		// Anim source preference:
		//   1. MovementManager-cached anim (set by HandleClientUpdate when EQEmu
		//      fires OP_ClientUpdate with the authoritative current_speed for
		//      MovementWalking vs MovementRunning).  This is the only source
		//      that correctly distinguishes a Playerbot RUNNING after its owner
		//      (RunTo, IsEngaged()==false) from a guard WALKING on patrol.
		//   2. Engaged-based heuristic — fallback when no recent ClientUpdate.
		//      Engaged NPC → run (EQEmu AI uses Mob::RunTo for pursuit), idle
		//      NPC → walk (Mob::WalkTo for roaming/patrolling).
		if (npc->IsMoving()) {
			int8_t cached = s.trilogy_client
			                    ? s.trilogy_client->GetRecentMovementAnim(
			                          static_cast<uint16_t>(npc->GetID()), now_ms)
			                    : static_cast<int8_t>(0);
			// Speed selection for both anim byte and wire velocity delta.
			// Priority: fleeing/feared → GetFearSpeed() (HP-ratio scaled at
			// mob.cpp:929-938: 1/3 walk at <5% HP, 1/2 walk at <15%,
			// walk+1 at <25%, 82% at <50%, full fearspeed >=50%).  Engaged
			// but not feared → runspeed.  Idle → walkspeed.
			//
			// IsFeared() at mob.h:1221 = (spellbonuses.IsFeared || flee_mode),
			// so this covers both fear-spell victims and combat low-HP fleers.
			//
			// Prior code used IsEngaged()?run:walk unconditionally, which
			// wire-encoded fleeing mobs at full runspeed (~50) even though the
			// server was moving them at ~8-14 u/s.  v29c client extrapolated
			// position ahead using the fake-fast delta → visible mob drifted
			// ~5× further along the flee path than the server actually was →
			// "can see but can't hit" until the next MoveToCommand 5 s refresh
			// (mob_movement_manager.cpp:184-194), HP-bucket speed transition
			// (mob.cpp:929-938 crossings), or OP_ClientTarget JIT drift-refresh
			// (trilogy_zone.cpp:2020-2069) snapped the client back to real pos,
			// producing the "sudden fast burst, briefly hittable, resume slow-
			// but-unhittable" loop.  Regression from PR#24 which removed the
			// moving-mob early-exit from SendMobHeartbeat.
			auto pick_speed = [&](Mob* n) -> int {
				if (n->IsFeared())  return n->GetFearSpeed();
				if (n->IsEngaged()) return n->GetRunspeed();
				return n->GetWalkspeed();
			};

			int eqemu_speed_for_delta = 0;
			if (cached != 0) {
				upd->anim_type = cached;
				// Cached anim byte is wire-encoded and not directly reversible;
				// re-derive server-side speed for the delta so wire velocity
				// matches actual server motion (see pick_speed comment above).
				eqemu_speed_for_delta = pick_speed(npc);
			} else if (npc->turning) {
				// RotateToCommand sets SetMoving(true) + turning=true and sends
				// MovementManager OP_ClientUpdate with animation=0 (rotation in
				// place — no position change).  HandleClientUpdate caches 0 for
				// these, but the heuristic below would still pick a walk byte
				// because IsMoving()==true.  Without this gate, hailed NPCs and
				// FaceTarget rotations play the walk cycle while spinning in
				// place — the user-visible "rotation polluted by start walking
				// animation" symptom.
				upd->anim_type = 0;
				// turning-in-place → no position translation, so delta stays 0.
			} else {
				const int eqemu_speed = pick_speed(npc);
				upd->anim_type        = EncodeTrilogyAnim(npc, eqemu_speed);
				eqemu_speed_for_delta = eqemu_speed;
			}

			// Velocity delta (option 3): give the v29c client a wire vector
			// to extrapolate position along between heartbeat snaps.  Without
			// this, the client only had heading + anim_type for between-update
			// motion, and anim_type-driven extrapolation undershoots EQEmu's
			// faster server speed → trajectory snap-forward each heartbeat.
			//
			// Derivation:
			//   eq_per_sec  = current_speed × 0.58   (mob_movement_manager.cpp:208)
			//   heading_rad = (GetHeading() / 512) × 2π   (EQEmu 0=N, 128=E, 256=S, 384=W)
			//   v_x         = sin(heading_rad) × eq_per_sec
			//   v_y         = cos(heading_rad) × eq_per_sec
			//   wire_dx     = v_x × kVelocityWireScale  (clamped to ±511)
			//
			// Calibration: compare the inbound delta log (what the v29c
			// player CLIENT sends for its own running motion) against the
			// outbound delta log (what we send for the closest moving NPC
			// here).  Adjust kVelocityWireScale until they line up in
			// magnitude for matched speeds.
			if (eqemu_speed_for_delta > 0) {
				const float heading_rad = (npc->GetHeading() / 512.0f)
				                          * 2.0f * 3.14159265f;
				const float eq_per_sec  = eqemu_speed_for_delta * 0.58f;
				const float vx          = std::sin(heading_rad) * eq_per_sec;
				const float vy          = std::cos(heading_rad) * eq_per_sec;
				const int32_t dx_wire   = static_cast<int32_t>(vx * kVelocityWireScale);
				const int32_t dy_wire   = static_cast<int32_t>(vy * kVelocityWireScale);
				WriteDeltaBitfield(upd, dx_wire, dy_wire, /*dz_wire=*/0);

				// Diagnostic: log the closest moving NPC at most once per
				// kDeltaDebugMs so we can read the values without flooding.
				if (now_ms - s.last_delta_dbg_out_ms >= kDeltaDebugMs &&
				    dist_sq < 400.0f * 400.0f) {
					s.last_delta_dbg_out_ms = now_ms;
					LogInfo("[Trilogy/delta OUT] npc=[{}] speed_int={} eq/s={:.1f} "
					        "heading={} anim={} dx={} dy={} scale={}",
					        npc->GetCleanName(),
					        eqemu_speed_for_delta,
					        eq_per_sec,
					        static_cast<int>(static_cast<uint8_t>(upd->heading)),
					        static_cast<int>(upd->anim_type),
					        dx_wire, dy_wire,
					        kVelocityWireScale);
				}
			}
		}

		// Turning NPCs bypass the outer-ring 500ms throttle so the user
		// gets smooth /hail rotation on a guard at 350u just as on one at 50u.
		if (!should_broadcast(upd, dist_sq, /*priority=*/npc->turning)) continue;
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
		float dist_sq = dx * dx + dy * dy;
		// No CULL_RADIUS_SQ skip: a living player wandering >600 u away
		// would lose their A120 stream, hit v29c's spawn staleness window
		// (~5-10 s), and silently vanish from this client's render — and
		// we don't re-issue 4921 NewSpawn on their return.  Outer ring
		// throttle (>300 u → 2 Hz) inside should_broadcast still caps
		// distant-player traffic.  Disconnect/zone-out still sends 2B20
		// via the normal cleanup path.

		auto* upd = reinterpret_cast<Trilogy::structs::SpawnPositionUpdate_Struct*>(
		                pkt + 4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct));
		memset(upd, 0, sizeof(*upd));

		float heading = c->GetHeading();
		upd->spawn_id = static_cast<int16_t>(c->GetID());
		upd->heading  = static_cast<int8_t>(static_cast<uint8_t>(heading / 2.0f));
		upd->y_pos    = static_cast<int16_t>(c->GetY());
		upd->x_pos    = static_cast<int16_t>(c->GetX());
		upd->z_pos    = static_cast<int16_t>(c->GetZ() * 10.0f);

		// delta_heading: relay what the turning client reported about itself,
		// exactly as EQClassic does for PCs (client_process.cpp:2390 stores
		// cu->delta_heading, mob.cpp:571 sends it back out).
		//
		// This is NOT the synthesised-value experiment warned about above.
		// That one computed a rate from successive headings for NPCs, with no
		// authoritative signal for when the turn ended, so v29c spun forever.
		// Here the value is the client's own, and the wire shows it returning
		// to 0 as soon as the player stops -- the stop is reported, not
		// inferred.  The getter's short staleness window is the backstop for
		// the case the client goes silent mid-turn.
		//
		// Measured need: a pivoting client reports ~3.2 times a second, so an
		// observer receiving absolute heading alone gets ~3 discrete facings
		// per second no matter how fast we broadcast.  This field is what
		// makes v29c sweep between them.
		if (c->IsTrilogyClient()) {
			upd->delta_heading =
				static_cast<TrilogyClient*>(c)->GetSelfReportedDeltaHeading(now_ms);
		}

		// Players self-render locally; this anim only matters for the other
		// observers receiving the broadcast.  Prefer the byte the moving
		// client reported for itself (captured in HandleClientUpdate) and
		// relay it verbatim, matching EQClassic's MakeSpawnUpdate
		// (mob.cpp:568 `spu->anim_type = animation`, where `animation` was
		// assigned from cu->anim_type on the way in).
		//
		// The previous `EncodeTrilogyAnim(c, c->GetRunspeed())` could not be
		// right for a human-driven entity: GetRunspeed() is unsigned, so the
		// encoded byte was ALWAYS positive (EncodeTrilogyAnim carries sign
		// through from its input) and always the same magnitude.  A player
		// running backward reports a NEGATIVE anim_type; re-deriving it
		// server-side threw that sign away and the observer's client played
		// the forward run cycle -- and extrapolated forward along heading --
		// while the position snaps pulled the character backward.
		//
		// It is passed through unmodified: this byte is already in v29c wire
		// units.  EncodeTrilogyAnim exists to map EQEmu speed integers INTO
		// that space and would corrupt a value already there.
		int8_t self_anim = 0;
		const bool have_self_anim =
			c->IsTrilogyClient() &&
			static_cast<TrilogyClient*>(c)->GetSelfReportedAnim(now_ms, self_anim);

		if (have_self_anim) {
			// Includes a reported 0 -- stopped, or strafing (v29c sends
			// anim_type=0 while strafing even though it is moving; see
			// EQClassic client_process.cpp:2396).
			upd->anim_type = self_anim;
		} else if (c->IsMoving()) {
			// No fresh self-report: a non-Trilogy client (Titanium et al.)
			// never sends a v29c anim byte, and a Trilogy client that has
			// gone quiet is past the staleness bound.  Fall back to the
			// previous synthesis.
			upd->anim_type = EncodeTrilogyAnim(c, c->GetRunspeed());
		}
		// delta_x/delta_y stay 0 — see the kVelocityWireScale note above;
		// EQClassic's /125 makes the relayed PC delta 0 in practice too.

		// A pivoting player takes the same priority path as a turning NPC:
		// without it the inner-ring 100 ms cap inside should_broadcast throws
		// away most of the rotation regardless of how fast the outer loop runs,
		// because heading is the only field changing and it changes on every
		// client update.
		const bool rotating =
			c->IsTrilogyClient() &&
			static_cast<TrilogyClient*>(c)->IsRotating(now_ms);

		if (!should_broadcast(upd, dist_sq, /*priority=*/rotating)) continue;
		if (++n == MAX_UPDATES_PER_PKT)
			flush_packet();
	}

	// Bots: same heartbeat pattern as players.  Bots live in entity_list.bot_list
	// (not npc_map and not client_map), so the loops above never see them.
	// Without this loop, Bot positions freeze at zone-in and the v29c staleness
	// timer eventually drops them entirely from the client's render.
	for (Bot* bot : entity_list.GetBotList()) {
		if (!bot) continue;

		float dx = bot->GetX() - s.pos_x;
		float dy = bot->GetY() - s.pos_y;
		float dist_sq = dx * dx + dy * dy;

		// Moving-non-turning bots WITHIN cull range are handled exclusively
		// by TrilogyClient::HandleClientUpdate (event-driven, batched into
		// FlushPendingMobUpdates).  Without this skip, moving bots produce
		// A120 traffic through BOTH paths — the dominant amplifier for the
		// 17-30 KB/s spikes observed when a Trilogy player moves with a
		// 70-bot raid follow (each bot fired A120 through both heartbeat
		// and event-driven, effectively doubling wire cost per moving bot).
		//
		// Turning bots MUST stay in heartbeat — RotateToCommand only fires
		// two OP_ClientUpdates for an entire rotation (start + end), which
		// leaves the client with nothing to render mid-rotation without
		// the heartbeat's nearby_turning bypass to kTurningThrottleMs.
		//
		// Bots OUTSIDE cull range MUST also stay in heartbeat: Test 11576
		// (2026-06-21) showed bots wandering >600 u away silently vanished
		// from v29c because we never re-issue OP_NewSpawn on return-to-range,
		// so once they age out of v29c's staleness window they stay invisible.
		// Event-driven culls at 600u too, so out-of-range bots need heartbeat
		// as their sole staleness-refresh path.
		if (bot->IsMoving() && !bot->turning && dist_sq <= CULL_RADIUS_SQ) {
			continue;
		}

		if (!bot->IsMoving()) {
			if ((now_ms / 100) % 5 != 0) continue;
		}

		auto* upd = reinterpret_cast<Trilogy::structs::SpawnPositionUpdate_Struct*>(
		                pkt + 4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct));
		memset(upd, 0, sizeof(*upd));

		upd->spawn_id = static_cast<int16_t>(bot->GetID());
		upd->heading  = static_cast<int8_t>(static_cast<uint8_t>(bot->GetHeading() / 2.0f));
		upd->y_pos    = static_cast<int16_t>(bot->GetY());
		upd->x_pos    = static_cast<int16_t>(bot->GetX());
		upd->z_pos    = static_cast<int16_t>(bot->GetZ() * 10.0f);
		// delta_heading stays 0 — v29c spin-stun semantics, not a smoothing hint.

		if (bot->IsMoving()) {
			// Prefer the MovementManager-cached anim (set by HandleClientUpdate
			// from the bot's RunTo/WalkTo invocations).  Without this, a bot
			// chasing its owner via RunTo gets overwritten with the walk byte
			// here because IsEngaged() is false for the follow-owner path.
			int8_t cached = s.trilogy_client
			                    ? s.trilogy_client->GetRecentMovementAnim(
			                          static_cast<uint16_t>(bot->GetID()), now_ms)
			                    : static_cast<int8_t>(0);
			if (cached != 0) {
				upd->anim_type = cached;
			} else if (bot->turning) {
				// Rotation in place — see NPC block above for rationale.
				upd->anim_type = 0;
			} else {
				const int eqemu_speed = bot->IsEngaged()
				                            ? bot->GetRunspeed()
				                            : bot->GetWalkspeed();
				upd->anim_type = EncodeTrilogyAnim(bot, eqemu_speed);
			}
		}

		if (!should_broadcast(upd, dist_sq, /*priority=*/bot->turning)) continue;
		if (++n == MAX_UPDATES_PER_PKT)
			flush_packet();
	}

	if (n > 0)
		flush_packet();
	// When no moving mobs are nearby, skip the send entirely.
	// The connection is kept alive by the client's own F320 stream, ACK responses,
	// and the 5-second stamina packet.

	// ============================================================
	// Stale-broadcast cache GC — local bookkeeping only, no wire effect.
	//
	// We keep `s.last_broadcast` per session to drive the dirty-flag gate in
	// should_broadcast.  Entries grow as mobs enter view; without trim, the
	// map keeps stale entries for mobs we'll never see again (mobs that died,
	// mobs we wandered far from).  Cap memory by dropping the cache entry
	// after kStaleTimeMs of no broadcast — but DO NOT send 2B20.
	//
	// The previous code (pre-2026-06-21 session 2096) proactively sent
	// OP_DeleteSpawn for "far away or quiet" mobs to bound v29c's entity
	// table, on the theory that v29c capped around 150-200 entries.  That
	// theory was unverified; the only thing it accomplished was orphaning
	// live mobs in the client's view: we 2B20'd a mob the moment it strayed
	// >800 u, never re-issued 4921 NewSpawn when it returned to range, then
	// sent 9F20/5820 attack packets for a spawn_id the client had forgotten.
	// Result: user-visible "NPC attacking invisible mob" symptom across
	// multiple test sessions (11576, 8716, 8352, 2096).  Removing the 2B20
	// also drops ~90 unnecessary ARQ packets per 10-min session, shrinking
	// the outbound-pending pressure that drives the gap-16 trap.
	//
	// Real death still emits 4A20+2B20 via HandleDeleteSpawn on the normal
	// path; zone-out clears the whole session.  Bot/Player exclusion no
	// longer matters since we never send the 2B20.
	//
	// 2026-06-27: ghost-spawn reconciliation — broadened scope.
	// Walks s.known_spawns (every spawn_id we've ever sent 0x4921 / 0x6121
	// for this session) and re-emits 2B20 for any whose serverside entity is
	// gone.  This catches phantoms that the engine's OP_DeleteSpawn
	// broadcast missed in the v29c outbound buffer (gap-16 orphan trap,
	// see [[project-trilogy-resend-explosion]]).
	//
	// First version was scoped to s.last_broadcast which only contains mobs
	// within CULL_RADIUS_SQ (600 u); that missed out-of-vision phantoms in
	// open zones like East Commonlands.  The known_spawns set covers
	// everything the client knows about.
	//
	// SAFE against [[project-trilogy-proactive-delete-bug]]: we only emit
	// 2B20 when entity_list.GetMob returns null — serverside ground truth,
	// never distance/time heuristic.  A live mob the player wandered away
	// from is still in entity_list and is never deleted.
	//
	// Throttled to ~2 s per session to keep wire pressure low.  At 500
	// known spawns per session this is one unordered_map lookup per entry,
	// ~250 lookups per second per session — trivial.  Number of 2B20s
	// emitted is exactly the count of real ghosts, no spillover.
	//
	// To disable if it causes problems: flip kEnableGhostReconcile to false.
	// ============================================================
	static constexpr bool     kEnableGhostReconcile      = true;
	static constexpr uint64_t kGhostReconcileIntervalMs  = 2000;
	// Drift-refresh: for out-of-cull mobs whose server position has drifted
	// from what we last broadcast to the client by more than this threshold,
	// send one A120 with the current server position.  Prevents the "client
	// renders a puma near the player but server has it 1400 u away" pathology
	// (the mob was in the zone-in bulk spawn but never entered player cull,
	// so heartbeat never refreshed it, so the client's render is frozen at
	// the zone-in position while the mob wandered arbitrarily far).
	//
	// 200 u threshold: at walkspeed ~20 u/s a mob traverses this in ~10 s,
	// so worst-case refresh cadence is ~0.1 Hz per drifting mob.  For 20
	// out-of-cull movers in a busy zone that's ~2 pps additional.  Client
	// render always tracks server truth to within 200 u — user sees mobs
	// walking away into the distance rather than a phantom stuck near them.
	//
	// Kill switch: flip kEnableDriftRefresh to false.
	static constexpr bool     kEnableDriftRefresh          = true;
	static constexpr int      kDriftRefreshThresholdUnits  = 200;

	// (No time-based last_broadcast GC — the map is bounded by known_spawns
	// which is itself bounded by NPC count, and ForgetKnownSpawn now erases
	// both.  Prior 15 s cache GC was incompatible with drift-refresh, which
	// needs the entry to persist for stationary out-of-cull mobs so a later
	// drift can be detected against a real baseline.)

	if (kEnableGhostReconcile &&
	    now_ms - s.last_ghost_reconcile_ms >= kGhostReconcileIntervalMs &&
	    !s.known_spawns.empty()) {
		s.last_ghost_reconcile_ms = now_ms;

		std::vector<uint16_t> ghosts;
		ghosts.reserve(8);

		// ── Position-stale diagnostic (log-only, no wire packets) ──────
		// Detects the second desync class: mob is ALIVE server-side but the
		// last position we broadcast to the v29c client is meaningfully
		// different from the mob's current server-side position.  User
		// symptom in open zones (lakeofillomen, commons): NPC "just stands
		// there" client-side; player runs up and attacks fail because the
		// server-side entity is elsewhere.  Ghost-reconcile handles the
		// "mob gone server-side" class; this catches the inverse.
		//
		// Trigger: current server XY differs from last broadcast wire XY
		// (int16, 1 EQ unit each) by more than kDesyncGapXY EQ units.
		// Per-spawn rate limit kDesyncLogIntervalMs prevents spamming a
		// mob that stays desynced for minutes.
		//
		// Zero wire packets emitted — pure LogInfo.  Reuses the same
		// known_spawns walk as ghost detection, no extra iteration cost.
		static constexpr int      kDesyncGapXY            = 50;    // EQ units
		static constexpr uint64_t kDesyncLogIntervalMs    = 10000; // per spawn

		for (uint16_t spawn_id : s.known_spawns) {
			// Skip the player's own spawn_id — sending 2B20 for self would
			// remove the player from their own client view.  Also skip 0,
			// which would not be in here normally but is harmless to guard.
			if (spawn_id == s.player_spawn_id || spawn_id == 0) continue;

			Mob* m = entity_list.GetMob(spawn_id);
			if (m == nullptr) {
				ghosts.push_back(spawn_id);
				continue;
			}

			auto lb_it = s.last_broadcast.find(spawn_id);
			if (lb_it == s.last_broadcast.end()) continue;
			auto& last = lb_it->second;

			int cur_wire_x = static_cast<int16_t>(m->GetX());
			int cur_wire_y = static_cast<int16_t>(m->GetY());
			int gap_x = cur_wire_x - static_cast<int>(last.x_pos);
			int gap_y = cur_wire_y - static_cast<int>(last.y_pos);
			int gap_sq = gap_x * gap_x + gap_y * gap_y;
			if (gap_sq <= kDesyncGapXY * kDesyncGapXY) continue;

			float dx_player = m->GetX() - s.pos_x;
			float dy_player = m->GetY() - s.pos_y;
			float dist_player_sq_local = dx_player * dx_player + dy_player * dy_player;
			const bool in_cull_local = dist_player_sq_local <= 600.0f * 600.0f;

			// ── Drift-refresh action (out-of-cull only) ──
			// The heartbeat loop above skips any mob beyond CULL_RADIUS_SQ, so
			// out-of-cull mobs never refresh through the normal path.  If such
			// a mob's server position has drifted by more than
			// kDriftRefreshThresholdUnits from the last position we broadcast,
			// emit one A120 with the current server position so the client's
			// render tracks server truth to within that threshold.  This is
			// the fix for the "puma renders near player, server has it 1400 u
			// away, attack fails" pathology.
			if (kEnableDriftRefresh && !in_cull_local &&
			    gap_sq >= kDriftRefreshThresholdUnits * kDriftRefreshThresholdUnits) {
				Trilogy::structs::SpawnPositionUpdate_Struct upd{};
				upd.spawn_id  = static_cast<int16_t>(spawn_id);
				upd.heading   = static_cast<int8_t>(
					static_cast<uint8_t>(m->GetHeading() / 2.0f));
				upd.y_pos     = static_cast<int16_t>(m->GetY());
				upd.x_pos     = static_cast<int16_t>(m->GetX());
				upd.z_pos     = static_cast<int16_t>(m->GetZ() * 10.0f);
				if (m->IsMoving()) {
					const int eqemu_speed = m->IsEngaged()
					                            ? m->GetRunspeed()
					                            : m->GetWalkspeed();
					upd.anim_type = EncodeTrilogyAnim(m, eqemu_speed);
				}

				uint8_t buf[4 + sizeof(upd)];
				int32_t n = 1;
				memcpy(buf, &n, 4);
				memcpy(buf + 4, &upd, sizeof(upd));
				SendApp(addr, port, s, ZN_OP_MobUpdate,
				        buf, static_cast<uint32_t>(sizeof(buf)),
				        /*ack_req=*/false);

				last.x_pos     = upd.x_pos;
				last.y_pos     = upd.y_pos;
				last.z_pos     = upd.z_pos;
				last.heading   = upd.heading;
				last.anim_type = upd.anim_type;
				last.sent_ms   = now_ms;

				LogInfo("[Trilogy drift-refresh] sid={} name='{}' "
				        "server=({:.1f},{:.1f},{:.1f}) drift={} dist_to_player={:.1f}",
				        static_cast<int>(spawn_id),
				        m->GetCleanName() ? m->GetCleanName() : "?",
				        m->GetX(), m->GetY(), m->GetZ(),
				        static_cast<int>(std::sqrt(static_cast<float>(gap_sq))),
				        std::sqrt(dist_player_sq_local));
				// Re-compute gap for the desync-pos log below now that we've
				// refreshed — will be near zero so log won't fire.
				gap_x = cur_wire_x - static_cast<int>(last.x_pos);
				gap_y = cur_wire_y - static_cast<int>(last.y_pos);
				gap_sq = gap_x * gap_x + gap_y * gap_y;
				if (gap_sq <= kDesyncGapXY * kDesyncGapXY) continue;
			}

			auto& log_ts = s.last_desync_log_ms[spawn_id];
			if (log_ts != 0 && now_ms - log_ts < kDesyncLogIntervalMs) continue;
			log_ts = now_ms;

			float dist_to_player = std::sqrt(dist_player_sq_local);

			// Diagnostic funnel — see comments in prior commit for field
			// meanings.  Reuses values already computed in this iteration.
			const uint64_t hb_age = now_ms - s.last_heartbeat_ms;
			const bool m_turning = m->IsNPC() && m->CastToNPC()->turning;
			const bool cadence_ok = m_turning || (now_ms / 100) % 5 == 0;
			const int cur_wire_z_int = static_cast<int16_t>(m->GetZ() * 10.0f);
			const bool would_be_dirty =
				(last.x_pos != cur_wire_x) ||
				(last.y_pos != cur_wire_y) ||
				(last.z_pos != cur_wire_z_int);

			LogInfo("[Trilogy desync-pos] sid={} name='{}' moving={} "
			        "client_last=({},{},{}/10) server=({:.1f},{:.1f},{:.1f}) "
			        "gap_xy={} age_ms={} dist_to_player={:.1f} "
			        "hb_age_ms={} cadence_ok={} in_cull={} would_dirty={}",
			        static_cast<int>(spawn_id),
			        m->GetCleanName() ? m->GetCleanName() : "?",
			        m->IsMoving() ? 1 : 0,
			        static_cast<int>(last.x_pos),
			        static_cast<int>(last.y_pos),
			        static_cast<int>(last.z_pos),
			        m->GetX(), m->GetY(), m->GetZ(),
			        static_cast<int>(std::sqrt(static_cast<float>(gap_sq))),
			        static_cast<uint64_t>(now_ms - last.sent_ms),
			        dist_to_player,
			        hb_age, cadence_ok ? 1 : 0,
			        in_cull_local ? 1 : 0, would_be_dirty ? 1 : 0);
		}

		for (uint16_t spawn_id : ghosts) {
			Trilogy::structs::DeleteSpawn_Struct ds{};
			ds.spawn_id    = static_cast<int16_t>(spawn_id);
			ds.ds_unknown1 = 0;
			SendToSession(SessionKey(addr, port), 0x2b20,
			              reinterpret_cast<const uint8_t*>(&ds),
			              static_cast<uint32_t>(sizeof(ds)));
			LogInfo("[Trilogy ghost-reconcile] 2B20 sid={} "
			        "(known to client, gone server-side, original delete lost)",
			        static_cast<int>(spawn_id));
			s.known_spawns.erase(spawn_id);
			// Drop any same-spawn-id residue from the position-broadcast cache.
			s.last_broadcast.erase(spawn_id);
			s.last_desync_log_ms.erase(spawn_id);
		}
	}
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
	// EQClassic-style "too many pending" outbound throttle.
	//
	// v29c's network layer (closest reference: EQClassic
	// Common/Source/EQPacketManager.cpp:209) silently orphans buffered ARQ
	// packets when one arrives ≥16 slots ahead of the last in-order processed,
	// and its CheckBufferedPackets is O(n²) per inbound packet — the orphan
	// list grows over session lifetime until the network thread CPU-starves
	// and outbound ARQ stops (the silent "cli_arq stuck" lock).  EQClassic's
	// SERVER avoids this by capping unacked ARQ packets at 9
	// (EQPACKETMANAGER_OUTBOUD_THRESHOLD) via a Sleep/wait loop on
	// IsTooMuchPending() before queueing more.  We replicate that gate here
	// but use the queue (no Sleep) since this is a single-threaded engine.
	//
	// Gate applies to ALL ARQ traffic including A120 heartbeats — the
	// previous per-tick A120 cap regressed time-to-lock (Test 8352
	// 2026-06-21, 8:35 vs 10:53 baseline) because tick-boundary smoothing
	// doesn't track what the client has actually acknowledged.
	//
	// For A120 specifically: when pending is at cap we DROP instead of
	// queueing.  The mob position in the queued packet would be stale by
	// the time it sends; next tick's heartbeat re-generates fresh data,
	// and the dirty-flag (last.sent_ms unchanged because should_broadcast
	// already returned true for the dropped packet — see below) is reset
	// on the next iteration anyway.
	{
		constexpr int kMaxUnacked = 9;
		const bool gatable = !s.draining_outbound
		                  && s.state == CONNECTED
		                  && ack_req
		                  && s.sack_init;
		if (gatable) {
			const int16_t unacked = static_cast<int16_t>(s.arq - s.acked_arq);
			if (unacked >= kMaxUnacked) {
				if (opcode == ZN_OP_MobUpdate) {
					// Drop stale heartbeat; next Tick will produce fresh coords.
					return;
				}
				constexpr size_t kQueueHardCap = 10000;
				if (s.outbound_queue.size() >= kQueueHardCap) {
					LogInfo("[TrilogyZone] outbound queue at hard cap ({}, unacked={}), dropping opcode={:04X} for char [{}]",
					        kQueueHardCap, unacked, opcode, s.char_name);
					return;
				}
				Session::QueuedAppPacket q;
				q.opcode  = opcode;
				if (plen > 0 && payload) q.payload.assign(payload, payload + plen);
				q.ack_req = ack_req;
				s.outbound_queue.push_back(std::move(q));
				return;
			}
		}
	}

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
		s.acked_arq = s.arq; // start with nothing pending (unacked = 0)
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
			uint16_t this_arq_frag = s.arq;
			{ uint16_t arq = htons(s.arq++); memcpy(buf + o, &arq, 2); o += 2; }
			{ uint16_t fs = htons(frag_group_seq);            memcpy(buf + o, &fs, 2); o += 2; }
			{ uint16_t fc = htons(static_cast<uint16_t>(i));  memcpy(buf + o, &fc, 2); o += 2; }
			{ uint16_t ft = htons(static_cast<uint16_t>(frags + 1)); memcpy(buf + o, &ft, 2); o += 2; }

			// EQClassic-faithful: asq_lo wraps freely (255 → 0), asq_hi
			// stays at its init value (1) for the entire session.  See
			// EQPacketManager.cpp:645-646 — only SACK.dbASQ_low++, never
			// touches SACK.dbASQ_high.  Earlier we carried the overflow
			// into asq_hi which diverged from Verant's wire pattern and
			// is the suspected cause of cumulative client state corruption
			// → 45-55 min "linkdead" wall (see [[project-trilogy-resend-
			// explosion]] 2026-06-23 ASQ-high investigation).
			if (has_asq) { buf[o++] = s.asq_hi; buf[o++] = s.asq_lo++; }
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

			// Symmetric outbound hex dump for fragmented packets, gated by
			// Netcode Detail.  Each fragment is dumped on its own line so we
			// can reassemble manually if needed.
			if (LogSys.IsLogEnabled(Logs::Detail, Logs::Netcode)) {
				std::string hex;
				int dump_len = std::min(o, 64);
				for (int k = 0; k < dump_len; ++k) {
					char tmp[4];
					snprintf(tmp, sizeof(tmp), "%02X ", buf[k]);
					hex += tmp;
				}
				LogNetcode("[TrilogyZone] tx FRAG datagram {}/{} {} bytes opcode={:04X} hdr={:02X}: {}",
				           i, frags, o, (i == 0 ? opcode : 0u), (unsigned)buf[0], hex);
			}

			// BWDiag: accumulate every fragment too — bandwidth-on-the-wire is
			// what matters, not logical packet count.
			s.bw_bytes_sent   += static_cast<uint64_t>(o);
			s.bw_packets_sent += 1;

			m_send_fn(addr, port, buf, static_cast<size_t>(o));

			// Stash fragment for EQClassic-style retransmit.  Each fragment
			// is its own ARQ packet, tracked independently.
			if (s.state == CONNECTED) {
				uint64_t now_ms_push = static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now().time_since_epoch()).count());
				Session::PendingArq pending;
				pending.arq           = this_arq_frag;
				pending.opcode        = opcode;
				pending.wire_bytes.assign(buf, buf + o);
				pending.send_count    = 1;
				pending.first_sent_ms = now_ms_push;
				pending.last_send_ms  = now_ms_push;
				pending.next_retry_ms = now_ms_push + 500;
				pending.backoff_ms    = 500;
				s.resend_queue.push_back(std::move(pending));
			}
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
	uint16_t this_arq = 0;
	if (ack_req) {
		this_arq = s.arq;
		uint16_t arq = htons(s.arq++);
		memcpy(buf + o, &arq, 2); o += 2;
	}
	// EQClassic wire format ([EQPacketManager.cpp:287-292, 549-560]):
	//   dbASQ_high is on the wire whenever a4_ASQ is set
	//   dbASQ_low is on the wire ONLY when a1_ARQ is also set
	// EQClassic bumps SACK.dbASQ_low on every a4_ASQ packet (line 645-646)
	// — BUT EQClassic forcibly overrides every QueuePacket to ack_req=true
	// (client.cpp:510), so it never tests the unreliable path.
	//
	// Empirically (2026-06-23 test log 4788), bumping asq_lo on unreliable
	// causes v29c to silently discard the *next reliable* packet (doors
	// don't render, /con replies don't appear, NPC interactions broken)
	// while still processing unreliable A120 fine. The signature is the
	// v29c client treating asq_lo as a *reliable-only* counter and rejecting
	// reliable packets whose asq_lo skipped ahead.
	//
	// Resolution: gate the increment on ack_req too. asq_lo becomes a
	// per-reliable counter (matching what the v29c client expects), wire
	// format still omits the low byte for unreliable per EQClassic. See
	// [[project-trilogy-unreliable-a120-wire-format]].
	buf[o++] = s.asq_hi;
	if (ack_req) {
		buf[o++] = s.asq_lo;
		// EQClassic-faithful: asq_lo wraps 255 → 0, asq_hi stays at its
		// init value (1) for the entire session.  See EQPacketManager.cpp:
		// 645-646 — only SACK.dbASQ_low++, never touches SACK.dbASQ_high.
		// Earlier we carried into asq_hi; that diverged from Verant's wire
		// pattern and is the suspected cause of cumulative client state
		// corruption → 45-55 min linkdead wall (see [[project-trilogy-
		// resend-explosion]] 2026-06-23 ASQ-high investigation).
		s.asq_lo++;
	}
	{ uint16_t op = htons(opcode); memcpy(buf + o, &op, 2); o += 2; }
	if (plen > 0 && payload) {
		if (static_cast<size_t>(o) + plen > sizeof(buf) - 4) return;
		memcpy(buf + o, payload, plen);
		o += static_cast<int>(plen);
	}
	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	// Symmetric outbound hex dump (matches OnDatagram's inbound dump format).
	// Gated by Netcode log level — enable when verifying wire-format correctness
	// against EQClassic / Verant captures.  At normal log levels this is a no-op.
	if (LogSys.IsLogEnabled(Logs::Detail, Logs::Netcode)) {
		std::string hex;
		int dump_len = std::min(o, 64);
		for (int i = 0; i < dump_len; ++i) {
			char tmp[4];
			snprintf(tmp, sizeof(tmp), "%02X ", buf[i]);
			hex += tmp;
		}
		LogNetcode("[TrilogyZone] tx datagram {} bytes opcode={:04X} hdr={:02X}: {}",
		           o, opcode, (unsigned)buf[0], hex);
	}

	// BWDiag: accumulate outbound bytes/packets — Tick emits the 5s roll.
	s.bw_bytes_sent   += static_cast<uint64_t>(o);
	s.bw_packets_sent += 1;

	m_send_fn(addr, port, buf, static_cast<size_t>(o));

	// Stash for per-packet exponential-backoff retransmit.  Skip when in
	// PM_FINISHING (SendClose) or when this is the close handshake itself.
	if (ack_req && s.state == CONNECTED && opcode != 0 /*close handshake*/) {
		uint64_t now_ms_push = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		Session::PendingArq pending;
		pending.arq           = this_arq;
		pending.opcode        = opcode;
		pending.wire_bytes.assign(buf, buf + o);
		pending.send_count    = 1;
		pending.first_sent_ms = now_ms_push;
		pending.last_send_ms  = now_ms_push;
		pending.next_retry_ms = now_ms_push + 500;
		pending.backoff_ms    = 500;
		s.resend_queue.push_back(std::move(pending));
	}
}

void TrilogyZoneServer::SendAck(const std::string& addr, int port, Session& s)
{
	if (!m_send_fn) return;

	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.acked_arq = s.arq; // start with nothing pending (unacked = 0)
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

	s.bw_bytes_sent   += static_cast<uint64_t>(o);
	s.bw_packets_sent += 1;

	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}

void TrilogyZoneServer::SendClose(const std::string& addr, int port, Session& s)
{
	if (!m_send_fn) return;

	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.acked_arq = s.arq; // start with nothing pending (unacked = 0)
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

	s.bw_bytes_sent   += static_cast<uint64_t>(o);
	s.bw_packets_sent += 1;

	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}

// ============================================================
// CompleteCamp — full camp-out flow for a Trilogy session
//
// Fired when the client sends ZN_OP_DeleteSpawn (0x5021) at T+30s from
// OP_Camp — the authoritative "camp finished" signal in the Verant-era
// protocol.  EQClassic's ProcessOP_DeleteSpawn does essentially the same
// (Save + Disconnect); we add the modern EQEmu bookkeeping (raid/group
// leave, guild online flag, merc save/depop).
//
// The SpawnAppearance(SAT_Camp=16=0x10, spawn_id=0, parameter=0) sent
// BEFORE SendClose is what tells the client to show the graceful camp-out
// transition instead of the "You have been disconnected" screen.  Same
// byte value used at zone-in for "here is your spawn id" — the client
// interprets it by current state (CONNECTING5 = self-id, CONNECTED = camp).
//
// Caller is responsible for RemoveSession(session_key) afterwards; this
// function does not remove the map entry so the caller's reference to
// `s` remains valid for the duration of the call.
void TrilogyZoneServer::CompleteCamp(uint64_t /*session_key*/, Session& s)
{
	if (!s.trilogy_client) return;

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

	// Graceful camp-out signal — must come BEFORE SendClose so the client
	// paints the camp transition rather than "You have been disconnected".
	{
		Trilogy::structs::SpawnAppearance_Struct sa{};
		memset(&sa, 0, sizeof(sa));
		sa.spawn_id  = 0;
		sa.type      = 0x10;   // SAT_Camp when applied to a CONNECTED session
		sa.parameter = 0;
		SendApp(s.source_addr, s.source_port, s, ZN_OP_Appearance,
		        reinterpret_cast<const uint8_t*>(&sa), sizeof(sa));
	}

	SendClose(s.source_addr, s.source_port, s);
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

		// Resolve material + color from the just-refreshed m_inv slot rather
		// than via tc->GetEquipmentMaterial / tc->GetEquipmentColor.  Those
		// helpers consult mob_texture_profile / armor_tint caches that the
		// v29c client itself populates when it sends OP_WearChange (via
		// HandleConnectedWearChange → Mob::WearChange → SetMobTextureProfile).
		// If the prior weapon's WearChange seeded the cache and the client
		// doesn't send a follow-up WearChange for the new weapon (or sends
		// one out of order with this MoveItem), the cache still holds the
		// OLD weapon's material and the helpers return it — so we'd echo the
		// old shortsword model back to the player and broadcast it to other
		// clients, defeating the swap.  Reading m_inv (already DB-synced by
		// refresh_slot above) is authoritative; an empty slot resolves to
		// material/color 0 which is the correct "bare hands" wire value.
		uint32 material = 0;
		uint32 color    = 0;
		if (const EQ::ItemInstance* w_inst = inv[vs.db_slot]) {
			if (const EQ::ItemData* w_item = w_inst->GetItem()) {
				if (strlen(w_item->IDFile) > 2 && Strings::IsNumber(&w_item->IDFile[2])) {
					material = Strings::ToUnsignedInt(&w_item->IDFile[2]);
				}
				// Normalise the "no tint" legacy sentinel (see NormalizeTintColor).
				color = Trilogy::NormalizeTintColor(
					w_inst->GetColor() ? w_inst->GetColor() : w_item->Color);
			}
		}

		// Keep the texture-profile cache coherent with what we're about to
		// put on the wire.  Without this, the very next reader of
		// GetEquipmentMaterial for this slot (spawn struct builds when a
		// new client zones in, pet summon, etc.) would still get the stale
		// pre-swap material.  Weapons aren't tinted so we deliberately
		// don't touch armor_tint here.
		tc->SetMobTextureProfile(vs.material_slot, material, color, 0);

		// Build the OP_WearChange packet by hand instead of calling
		// Mob::WearChange — the latter also writes armor_tint (which would
		// clobber tinted armor for non-weapon slots if this helper were ever
		// reused for them).  We only want the wire effect plus the texture-
		// profile coherency above.
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
		// Update v29c-client-known-material model for the player's own slot.
		tc->RecordKnownMaterial(
			static_cast<uint16_t>(tc->GetPlayerSpawnId()),
			static_cast<uint8_t>(vs.material_slot),
			static_cast<uint8_t>(material & 0xFF));
	}
}

// ============================================================
// ApplyWornSlotPickupSideEffects — sync m_inv + fire unequip side-effects for
// the whole-stack pickup path when the source is a worn slot (1-20 or ammo).
//
// The whole-stack pickup branch of HandleMoveItem defers the DB row movement
// until the drop step (avoids one UPDATE if the player drops back to the same
// slot).  RefreshWornSlotsAfterMove is not appropriate here because it re-reads
// the DB — and the DB row is still at the worn slot at pickup time, so it would
// silently restore the just-unequipped item.
//
// This helper mirrors the essential CalcBonuses / ApplyWeaponsStance /
// SetAttackTimer / WearChange / EVENT_UNEQUIP tail of RefreshWornSlotsAfterMove
// but skips the DB re-read — it directly pops the ItemInstance out of m_inv at
// the worn slot and stashes it at m_inv[slotCursor] so lore/CalcBonuses and any
// engine code that consults m_inv sees the cursor state instead of the stale
// equipped state.
//
// Consequences of NOT calling this (the pre-fix behavior):
//   • m_inv[Primary/Secondary] still points at the weapon, so Client::Attack
//     (attack.cpp:1618) finds a non-null weapon → AttackAnimation returns the
//     weapon skill (1H Slash / Piercing / …) instead of HandtoHand.
//   • CalcBonuses is not called, so has_two_hander_equipped, weapon damage
//     bonuses, and haste stay applied while the item is on the cursor.
//   • SetAttackTimer is not called, so the primary/secondary attack timers
//     stay calibrated to the weapon delay instead of GetHandToHandDelay().
//   • No OP_WearChange, so the character continues to visually swing the
//     weapon in other players' views.
// ============================================================
void TrilogyZoneServer::ApplyWornSlotPickupSideEffects(Session& s, int from_db)
{
	if (!s.trilogy_client) return;

	auto is_worn = [](int slot) {
		return (slot >= 1 && slot <= 20) || slot == EQ::invslot::slotAmmo;
	};
	if (!is_worn(from_db)) return;

	auto* tc  = s.trilogy_client;
	auto& inv = tc->GetInv();

	// Pop the ItemInstance out of the worn slot.  If nothing was there (e.g.
	// prior desync or race) we still run the recalc tail below so the client's
	// bonuses/attack-timer end up coherent with an empty slot.
	EQ::ItemInstance* worn_inst = inv.PopItem(static_cast<int16>(from_db));
	uint32 worn_id = worn_inst ? worn_inst->GetItem()->ID : 0;

	// Fire EVENT_UNEQUIP_ITEM for whatever was equipped.  Mirrors the
	// unequip side of RefreshWornSlotsAfterMove::fire_unequip.
	if (worn_inst && worn_inst->GetItem()) {
		if (parse->ItemHasQuestSub(worn_inst, EVENT_UNEQUIP_ITEM)) {
			parse->EventItem(EVENT_UNEQUIP_ITEM, tc, worn_inst, nullptr, "", from_db);
		}
		if (parse->PlayerHasQuestSub(EVENT_UNEQUIP_ITEM_CLIENT)) {
			parse->EventPlayer(EVENT_UNEQUIP_ITEM_CLIENT, tc,
			                   fmt::format("1 {}", from_db), worn_id);
		}
	}

	// Stash on cursor so m_inv[slotCursor] matches the logical state (item is
	// on the cursor).  Any existing cursor item is displaced silently — the
	// v29c client can only hold one thing on the cursor at a time, so a
	// well-behaved client won't reach this branch with a pre-populated cursor.
	if (worn_inst) {
		auto* prev = inv.PopItem(EQ::invslot::slotCursor);
		safe_delete(prev);
		inv.PutItem(EQ::invslot::slotCursor, *worn_inst);
	}

	tc->CalcBonuses();
	tc->ApplyWeaponsStance();

	// Weapon/range slot touched → recalibrate both attack timers immediately
	// so the very next auto-attack tick uses GetHandToHandDelay().  Without
	// this, the still-set weapon-delay trigger keeps firing at the wrong
	// cadence until the next weapon-slot event.
	if (from_db == EQ::invslot::slotPrimary ||
	    from_db == EQ::invslot::slotSecondary ||
	    from_db == EQ::invslot::slotRange) {
		tc->SetAttackTimer();
	}

	// Weapon-visual clear for slot 13/14.  v29c has no visual for the range
	// slot, so a bow pickup skips this block (matches RefreshWornSlotsAfterMove).
	// The wire pattern (material=0, color=0, self + broadcast, texture-profile
	// cache update, RecordKnownMaterial) mirrors the tail of that helper — the
	// only differences are that (a) we already know material/color are zero
	// (bare hands) and (b) we don't need to consult m_inv for the "after" state.
	uint8_t material_slot = 0xFF;
	if (from_db == EQ::invslot::slotPrimary)   material_slot = EQ::textures::weaponPrimary;
	if (from_db == EQ::invslot::slotSecondary) material_slot = EQ::textures::weaponSecondary;
	if (material_slot != 0xFF) {
		tc->SetMobTextureProfile(material_slot, 0, 0, 0);

		auto* outapp = new EQApplicationPacket(OP_WearChange, sizeof(::WearChange_Struct));
		auto* w = reinterpret_cast<::WearChange_Struct*>(outapp->pBuffer);
		w->spawn_id         = tc->GetID();
		w->material         = 0;
		w->elite_material   = 0;
		w->hero_forge_model = 0;
		w->color.Color      = 0;
		w->wear_slot_id     = material_slot;
		entity_list.QueueClients(tc, outapp, true);
		safe_delete(outapp);

		using TrilWC = Trilogy::structs::WearChange_Struct;
		TrilWC wc{};
		wc.spawn_id     = static_cast<int32_t>(tc->GetPlayerSpawnId());
		wc.wear_slot_id = static_cast<int8_t>(material_slot);
		wc.slot_graphic = 0;
		wc.sub_op       = 0;
		wc.color        = 0;
		wc.wc_unknown3  = 0;
		wc.flag         = 0;
		SendApp(s.source_addr, s.source_port, s, 0x9220,
		        reinterpret_cast<const uint8_t*>(&wc),
		        static_cast<uint32_t>(sizeof(wc)));
		tc->RecordKnownMaterial(
			static_cast<uint16_t>(tc->GetPlayerSpawnId()),
			material_slot, 0);
	}

	LogInfo("[TrilogyZone] worn-slot pickup char={} slot={} item={} → cursor "
	        "(m_inv synced, CalcBonuses+SetAttackTimer fired, visual cleared)",
	        s.char_id, from_db, worn_id);

	safe_delete(worn_inst);
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
		HandleTradeMoveItem(s, from_wire, to_wire, mi->number_in_stack);
		return;
	}

	// Tradeskill / world-container interior: wire slots 4000-4009 are the 10
	// "station" slots inside whatever forge/oven/box the player clicked open.
	// Items here live on the Object (m_tradeskill_object->m_inst as a bag),
	// NOT in the inventory DB, so the regular wire_to_db path can't touch them.
	// All non-trivial moves go through cursor (wire 0) in v29c, so the only
	// real shapes are:
	//   from=4000..4009, to=0       → take item out of station → cursor
	//   from=0,         to=4000..4009 → put cursor item → station
	//   from=4000..4009, to=4000..4009 → reorder inside station (rare)
	const bool from_is_world = (from_wire >= 4000 && from_wire <= 4009);
	const bool to_is_world   = (to_wire   >= 4000 && to_wire   <= 4009);
	if (from_is_world || to_is_world) {
		Object* obj = s.trilogy_client ? s.trilogy_client->GetTradeskillObject() : nullptr;
		if (!obj) {
			LogInfo("[TrilogyZone] MoveItem char={} world-slot move but no tradeskill object open "
			        "(from={} to={}); ignoring", s.char_id, from_wire, to_wire);
			return;
		}

		auto insert_cursor_row = [&](uint32 item_id, int16 charges, uint32 color) {
			// Use DB slot 33 (cursor) for the freshly extracted item.  If it's
			// occupied push to the queue at 8000-8010 (mirrors EQEmu's cursor
			// queue behaviour for arbitrary item arrivals; see project memory
			// `project_trilogy_cursor_queue`).
			auto occ = database.QueryDatabase(fmt::format(
			    "SELECT COUNT(*) FROM `inventory` WHERE `charid`={} AND `slotid`=33", s.char_id));
			int cursor_slot = 33;
			if (occ.Success() && occ.RowCount() > 0 && Strings::ToInt(occ.begin()[0]) > 0) {
				auto q = database.QueryDatabase(fmt::format(
				    "SELECT COALESCE(MAX(`slotid`),7999)+1 FROM `inventory` WHERE `charid`={} AND "
				    "`slotid` BETWEEN 8000 AND 8010", s.char_id));
				if (q.Success() && q.RowCount() > 0) {
					cursor_slot = static_cast<int>(Strings::ToInt(q.begin()[0]));
					if (cursor_slot > 8010) cursor_slot = 8010;
				}
			}
			database.QueryDatabase(fmt::format(
			    "REPLACE INTO `inventory` (`charid`,`slotid`,`itemid`,`charges`,`color`) "
			    "VALUES ({},{},{},{},{})",
			    s.char_id, cursor_slot, item_id, charges, color));
			return cursor_slot;
		};

		if (from_is_world && to_wire == 0) {
			// Take item out of station → cursor.
			const uint8 idx = static_cast<uint8>(from_wire - 4000);
			EQ::ItemInstance* inst = obj->PopItem(idx);
			if (!inst) {
				LogInfo("[TrilogyZone] MoveItem char={} world-pop idx={} empty", s.char_id, idx);
				return;
			}
			const EQ::ItemData* item = inst->GetItem();
			if (item) {
				const int cur_slot = insert_cursor_row(item->ID,
				                                      static_cast<int16>(inst->GetCharges()),
				                                      inst->GetColor());
				s.cursor_from_db = cur_slot;
				LogInfo("[TrilogyZone] MoveItem char={} station[{}] → cursor (DB slot {}) item={}",
				        s.char_id, idx, cur_slot, item->ID);
			}
			obj->Save();
			safe_delete(inst);
			return;
		}

		if (from_wire == 0 && to_is_world) {
			// Place cursor item → station.  Resolve the source DB row the same
			// way the regular drop-from-cursor path does.
			int src_db = s.cursor_from_db;
			if (src_db < 0) {
				auto r = database.QueryDatabase(fmt::format(
				    "SELECT `slotid` FROM `inventory` WHERE `charid`={} AND "
				    "(`slotid`=33 OR (`slotid` BETWEEN 8000 AND 8010)) "
				    "ORDER BY `slotid` ASC LIMIT 1", s.char_id));
				if (r.Success() && r.RowCount() > 0)
					src_db = static_cast<int>(Strings::ToInt(r.begin()[0]));
			}
			if (src_db < 0) {
				LogInfo("[TrilogyZone] MoveItem char={} drop-to-station but no cursor row found",
				        s.char_id);
				return;
			}

			auto r = database.QueryDatabase(fmt::format(
			    "SELECT `itemid`,`charges`,`color` FROM `inventory` "
			    "WHERE `charid`={} AND `slotid`={}", s.char_id, src_db));
			if (!r.Success() || r.RowCount() == 0) return;
			auto row = r.begin();
			const uint32 item_id = static_cast<uint32>(Strings::ToInt(row[0]));
			const int16  charges = static_cast<int16>(Strings::ToInt(row[1]));
			const uint32 color   = static_cast<uint32>(Strings::ToInt(row[2]));
			if (item_id == 0) return;

			EQ::ItemInstance* inst = database.CreateItem(item_id, charges);
			if (!inst) return;
			inst->SetColor(color);

			const uint8 idx = static_cast<uint8>(to_wire - 4000);
			// Swap: if the target slot is occupied, pop existing first and
			// stash it on cursor instead of the just-placed item.
			EQ::ItemInstance* existing = obj->PopItem(idx);
			obj->PutItem(idx, inst);
			database.QueryDatabase(fmt::format(
			    "DELETE FROM `inventory` WHERE `charid`={} AND `slotid`={}",
			    s.char_id, src_db));
			s.cursor_from_db = -1;
			LogInfo("[TrilogyZone] MoveItem char={} cursor (DB slot {}) → station[{}] item={}",
			        s.char_id, src_db, idx, item_id);
			safe_delete(inst);

			if (existing) {
				const EQ::ItemData* eitem = existing->GetItem();
				if (eitem) {
					const int cur_slot = insert_cursor_row(eitem->ID,
					                                      static_cast<int16>(existing->GetCharges()),
					                                      existing->GetColor());
					s.cursor_from_db = cur_slot;
					LogInfo("[TrilogyZone] MoveItem char={} station[{}] swap-out → cursor (DB slot {}) item={}",
					        s.char_id, idx, cur_slot, eitem->ID);
				}
				safe_delete(existing);
			}

			obj->Save();
			return;
		}

		if (from_is_world && to_is_world) {
			// Reorder inside the station.
			const uint8 src = static_cast<uint8>(from_wire - 4000);
			const uint8 dst = static_cast<uint8>(to_wire   - 4000);
			EQ::ItemInstance* a = obj->PopItem(src);
			EQ::ItemInstance* b = obj->PopItem(dst);
			if (a) obj->PutItem(dst, a);
			if (b) obj->PutItem(src, b);
			obj->Save();
			safe_delete(a);
			safe_delete(b);
			return;
		}

		// Other combinations (world ↔ non-cursor inventory) shouldn't happen
		// in v29c — the client always routes through cursor.  Drop quietly.
		LogInfo("[TrilogyZone] MoveItem char={} unsupported world-slot shape from={} to={}",
		        s.char_id, from_wire, to_wire);
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
		// Picking up to cursor.  Resolve the DB slot now.
		const int from_db = wire_to_db(from_wire);
		if (from_db < 0) return;

		// ── Partial-stack pickup ────────────────────────────────────────────
		// v29c sends MoveItem_Struct.number_in_stack > 0 when the client is
		// splitting a stack (e.g. right-click pickup of 5 from a stack of 20).
		// EQClassic ProcessOP_MoveItem (client_process.cpp:1719) and stock
		// EQEmu Client::SwapItem (inventory.cpp:2138-2148) both decrement the
		// source row and put the partial on cursor at pickup time.  Without
		// this:
		//   • the cursor "represents" the whole source row, so any subsequent
		//     drop moves/swaps the ENTIRE row (corrupting same-item moves);
		//   • trade staging reads the full DB row, and giving the trade to a
		//     quest NPC deletes the whole row — losing the leftover the
		//     player never intended to give.
		// Materialise the partial as a real cursor row (DB slot 33, or the
		// 8000-8010 queue if 33 is occupied) so the rest of the pipeline
		// (drop / trade / destroy) sees the correct partial qty.
		if (mi->number_in_stack > 0) {
			auto src_q = database.QueryDatabase(fmt::format(
			    "SELECT `itemid`, `charges`, `color` FROM `inventory` "
			    "WHERE `charid`={} AND `slotid`={}",
			    s.char_id, from_db));
			if (src_q.Success() && src_q.RowCount() > 0) {
				auto row = src_q.begin();
				const uint32 src_iid = static_cast<uint32>(Strings::ToInt(row[0]));
				const int16  src_chg = static_cast<int16>(Strings::ToInt(row[1]));
				const uint32 src_col = static_cast<uint32>(Strings::ToInt(row[2]));
				const int16  pick    = static_cast<int16>(mi->number_in_stack);

				if (src_iid != 0 && src_chg > pick) {
					// Pick a free cursor DB slot: 33 if empty, else the next
					// 8000-8010 queue slot (matches the world-container path
					// just above and the cursor-queue convention).
					int cursor_slot = 33;
					auto occ_q = database.QueryDatabase(fmt::format(
					    "SELECT COUNT(*) FROM `inventory` "
					    "WHERE `charid`={} AND `slotid`=33", s.char_id));
					if (occ_q.Success() && occ_q.RowCount() > 0 &&
					    Strings::ToInt(occ_q.begin()[0]) > 0) {
						auto next_q = database.QueryDatabase(fmt::format(
						    "SELECT COALESCE(MAX(`slotid`),7999)+1 FROM `inventory` "
						    "WHERE `charid`={} AND `slotid` BETWEEN 8000 AND 8010",
						    s.char_id));
						if (next_q.Success() && next_q.RowCount() > 0) {
							cursor_slot = static_cast<int>(Strings::ToInt(next_q.begin()[0]));
							if (cursor_slot > 8010) cursor_slot = 8010;
						}
					}

					database.QueryDatabase(fmt::format(
					    "UPDATE `inventory` SET `charges`=`charges`-{} "
					    "WHERE `charid`={} AND `slotid`={}",
					    pick, s.char_id, from_db));
					database.QueryDatabase(fmt::format(
					    "REPLACE INTO `inventory` (`charid`,`slotid`,`itemid`,`charges`,`color`) "
					    "VALUES ({},{},{},{},{})",
					    s.char_id, cursor_slot, src_iid, pick, src_col));

					s.cursor_from_db           = cursor_slot;
					s.cursor_partial_origin_db = from_db;

					// m_inv sync: source slot charges changed; cursor row may
					// be new.  Re-read both so engine code that consults m_inv
					// (lore checks, CalcBonuses, etc.) doesn't see stale data.
					if (s.trilogy_client) {
						auto& inv = s.trilogy_client->GetInv();
						auto sync_one = [&](int db_slot) {
							auto* old = inv.PopItem(static_cast<int16>(db_slot));
							safe_delete(old);
							auto r = database.QueryDatabase(fmt::format(
							    "SELECT `itemid`,`charges`,`color` FROM `inventory` "
							    "WHERE `charid`={} AND `slotid`={}",
							    s.char_id, db_slot));
							if (!r.Success() || r.RowCount() == 0) return;
							auto rrow = r.begin();
							const uint32 iid = static_cast<uint32>(Strings::ToInt(rrow[0]));
							if (iid == 0) return;
							const int16  ch  = static_cast<int16>(Strings::ToInt(rrow[1]));
							const uint32 col = static_cast<uint32>(Strings::ToInt(rrow[2]));
							EQ::ItemInstance* inst = database.CreateItem(iid, ch);
							if (!inst) return;
							inst->SetColor(col);
							inv.PutItem(static_cast<int16>(db_slot), *inst);
							safe_delete(inst);
						};
						sync_one(from_db);
						sync_one(cursor_slot);
					}

					LogInfo("[TrilogyZone] MoveItem (partial pickup) char={} from_wire={} "
					        "src_db={} src_remaining={} cursor_db={} partial_chg={} item={}",
					        s.char_id, from_wire, from_db, (src_chg - pick),
					        cursor_slot, pick, src_iid);
					return;
				}
			}
		}

		// Whole-stack pickup (n_i_s == 0, or n_i_s ≥ src.charges = degenerate):
		// keep the existing optimisation — record source slot, defer the row
		// movement until drop.  Drop path handles row UPDATE / merge / swap.
		LogInfo("[TrilogyZone] MoveItem (pick up) char={} from_wire={} → cursor, "
		        "cursor_from_db={} client_number_in_stack={}",
		        s.char_id, from_wire, from_db, mi->number_in_stack);
		s.cursor_from_db           = from_db;
		s.cursor_partial_origin_db = -1;

		// If we just picked up from a worn slot (equip → cursor), the DB row
		// stays put but m_inv, bonuses, attack timer, and the weapon visual all
		// need to be updated NOW — otherwise the auto-attack loop keeps swinging
		// the still-referenced weapon with its weapon skill mid-combat.
		ApplyWornSlotPickupSideEffects(s, from_db);
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
			from_db          = s.cursor_from_db;
			s.cursor_from_db = -1;
		}
		// Drop from cursor terminates the partial-pickup origin tracking — the
		// cursor row is about to be merged/moved/destroyed regardless of where
		// it lands.
		s.cursor_partial_origin_db = -1;
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

	// Stack-merge probe: if both slots hold the same stackable item, MERGE
	// charges (cap at StackSize, overflow stays at source) instead of swapping.
	// Catches both the partial-pickup-then-drop-onto-existing-stack case (cursor
	// row has the partial qty, dest has the original stack) and whole-stack
	// pickup dropped onto another same-item stack.  Without this, the swap
	// path moved entire rows around silently, mangling the per-slot counts the
	// client expected after the merge.
	bool did_stack_merge = false;
	if (to_occupied) {
		auto src_q = database.QueryDatabase(fmt::format(
		    "SELECT `itemid`, `charges` FROM `inventory` "
		    "WHERE `charid`={} AND `slotid`={}", s.char_id, from_db));
		auto dst_q = database.QueryDatabase(fmt::format(
		    "SELECT `itemid`, `charges` FROM `inventory` "
		    "WHERE `charid`={} AND `slotid`={}", s.char_id, to_db));
		if (src_q.Success() && src_q.RowCount() > 0 &&
		    dst_q.Success() && dst_q.RowCount() > 0) {
			auto srow = src_q.begin();
			auto drow = dst_q.begin();
			const uint32 src_iid = static_cast<uint32>(Strings::ToInt(srow[0]));
			const int16  src_chg = static_cast<int16>(Strings::ToInt(srow[1]));
			const uint32 dst_iid = static_cast<uint32>(Strings::ToInt(drow[0]));
			const int16  dst_chg = static_cast<int16>(Strings::ToInt(drow[1]));
			if (src_iid != 0 && src_iid == dst_iid) {
				const EQ::ItemData* item = database.GetItem(src_iid);
				if (item && item->Stackable && item->StackSize > 1) {
					const int stack_max = item->StackSize;
					const int total     = src_chg + dst_chg;
					if (total <= stack_max) {
						database.QueryDatabase(fmt::format(
						    "UPDATE `inventory` SET `charges`={} "
						    "WHERE `charid`={} AND `slotid`={}",
						    total, s.char_id, to_db));
						database.QueryDatabase(fmt::format(
						    "DELETE FROM `inventory` "
						    "WHERE `charid`={} AND `slotid`={}",
						    s.char_id, from_db));
					} else {
						database.QueryDatabase(fmt::format(
						    "UPDATE `inventory` SET `charges`={} "
						    "WHERE `charid`={} AND `slotid`={}",
						    stack_max, s.char_id, to_db));
						database.QueryDatabase(fmt::format(
						    "UPDATE `inventory` SET `charges`={} "
						    "WHERE `charid`={} AND `slotid`={}",
						    total - stack_max, s.char_id, from_db));
					}
					LogInfo("[TrilogyZone] MoveItem char={} stack-merge item={} "
					        "src_db={} ({}) + dst_db={} ({}) → dst {} (stack_max={})",
					        s.char_id, src_iid, from_db, (int)src_chg,
					        to_db, (int)dst_chg,
					        (total <= stack_max ? total : stack_max), stack_max);
					did_stack_merge = true;
				}
			}
		}
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
	} else if (!did_stack_merge) {
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
	//
	// Casting-slot translation.  v29c and modern EQEmu agree on the spell gems
	// (0..7 -> Gem1..Gem8) and on nothing else:
	//
	//   v29c SLOT_ITEMSPELL = 10, but EQEmu's CastingSlot::Gem11 is also 10 and
	//   CastingSlot::Item is 22 (emu_constants.h:417-422).  Forwarding the raw 10
	//   made Handle_OP_CastSpell read every item click as an 11th spell gem, which
	//   trips its Mnemonic Retention guard — ValueWithin(slot, 8, 11) against
	//   GetAA(aaMnemonicRetention) — and a Trilogy character has no AAs.  Result:
	//   every clicky in the game died on "You do not have the required AA to use
	//   this spell slot", before inventoryslot was ever looked at.
	//
	//   v29c slot 9 (the Ability button) is deliberately NOT mapped to
	//   CastingSlot::Ability here.  Lay on Hands and Harm Touch are the only two
	//   abilities that use it and both are intercepted above, precisely because
	//   the modern Ability path emits a packet burst that crashes v29c.  Leaving 9
	//   unmapped keeps anything unexpected out of that path.
	auto to_emu_casting_slot = [](uint32 wire) -> uint32 {
		if (wire == 10) return static_cast<uint32>(EQ::spells::CastingSlot::Item);
		return wire; // gems 0..7 are identity; 9 handled by the intercepts above
	};

	auto* app = new EQApplicationPacket(OP_CastSpell, sizeof(::CastSpell_Struct));
	auto* emu = reinterpret_cast<::CastSpell_Struct*>(app->pBuffer);
	memset(emu, 0, sizeof(::CastSpell_Struct));

	emu->slot     = to_emu_casting_slot(static_cast<uint32>(tri->slot));
	emu->spell_id = static_cast<uint32>(tri->spell_id);

	// Item clicks carry a wire inventory slot, which needs the same translation
	// every other inbound path in this file already uses (wire 0 = cursor, 21-29
	// general shift by one, 250-339 bag contents shift by one).  Gem casts leave
	// the field unused, so only translate when it actually addresses an item.
	if (emu->slot == static_cast<uint32>(EQ::spells::CastingSlot::Item)) {
		const int emu_slot = TrilogyWireSlotToEmuSlot(
		    static_cast<uint32_t>(static_cast<uint16>(tri->inventoryslot)),
		    s.cursor_from_db);
		if (emu_slot < 0) {
			LogInfo("[TrilogyZone] CastSpell: char={} item click from unmappable "
			        "wire slot {} - dropped", s.char_name,
			        static_cast<uint16>(tri->inventoryslot));
			delete app;
			return;
		}
		emu->inventoryslot = static_cast<uint32>(emu_slot);
		LogInfo("[TrilogyZone] CastSpell: char={} item click wire_slot={} -> emu_slot={} spell={}",
		        s.char_name, static_cast<uint16>(tri->inventoryslot), emu_slot, spell_id);
	} else {
		emu->inventoryslot = static_cast<uint32>(static_cast<uint16>(tri->inventoryslot));
	}

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

	// Diagnostics for bot-follow-on-zone: report bot/group state before the
	// standard handler runs (it invokes Bot::ProcessClientZoneChange which
	// expects the owner's bots to be present in entity_list and grouped with
	// the owner so each can Save()+Depop() — see bot.cpp:7222).
	{
		auto bots_in_zone = entity_list.GetBotsByBotOwnerCharacterID(
		    s.trilogy_client->CharacterID());
		Group* g = s.trilogy_client->GetGroup();
		LogInfo("[TrilogyZP] ZoneChange bot-follow PRE | char='{}' charid={} "
		        "bots_in_entity_list={} has_group={} group_id={} bots_enabled={}",
		        s.trilogy_client->GetName(), s.trilogy_client->CharacterID(),
		        (unsigned)bots_in_zone.size(),
		        g != nullptr, g ? g->GetID() : 0,
		        RuleB(Bots, Enabled) ? "Y" : "N");
		for (auto* b : bots_in_zone) {
			if (!b) continue;
			LogInfo("[TrilogyZP] ZoneChange bot-follow PRE   bot='{}' bot_id={} "
			        "has_group={} group_member_of_owner={}",
			        b->GetName(), b->GetBotID(), b->HasGroup(),
			        (g && g->IsGroupMember(b)));
		}
	}

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

		// Nullify the player's slot in any in-zone Group/Raid BEFORE destroying
		// the Client.  The normal Titanium path leaves the Client object alive
		// for the duration of the worldserver round-trip (and any auxiliary
		// MemberZoned call cleans up the slot before delete), but the Trilogy
		// path destroys the entity synchronously here.  Without this step, the
		// Group's members[0] points to freed memory; the next NPC death in this
		// zone (~Mob -> EntityList::UnMarkNPC -> Group::UpdateXTargetMarkedNPC)
		// dereferences it via members[i]->IsClient() and crashes the zone.
		// Bot slots are already nullified by Bot::Zone() (called from
		// Bot::ProcessClientZoneChange at the top of Handle_OP_ZoneChange).
		if (Group* g = s.trilogy_client->GetGroup()) {
			g->MemberZoned(s.trilogy_client);
		}
		if (Raid* r = s.trilogy_client->GetRaid()) {
			r->MemberZoned(s.trilogy_client);
		}

		s.trilogy_client  = nullptr;
		s.eqemu_entity_id = 0;
		entity_list.RemoveMob(id);
		s.counted_in_zone = false;
		LogInfo("[TrilogyZone] {} entity removed on zone-out, numclients={}", s.char_name, numclients);
	}
}
