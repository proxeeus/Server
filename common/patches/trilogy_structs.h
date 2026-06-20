/*	EQEMu: Everquest Server Emulator

	Copyright (C) 2001-2016 EQEMu Development Team (http://eqemulator.net)

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

// Trilogy (EverQuest v29c/v30, build "8-09-2001 14:25") packet structures.
// All structures validated against EQClassic source:
//   Common/Include/eq_packet_structs.h
//   Common/Include/PlayerProfile.h
//   LS/Login/login_structs.h

#ifndef COMMON_TRILOGY_STRUCTS_H
#define COMMON_TRILOGY_STRUCTS_H

namespace Trilogy
{
	namespace structs {

// Slot / array limits sourced from EQClassic PlayerProfile.h / config.h
static const uint32 BUFF_COUNT           = 15;   // visible buff slots in PlayerProfile
static const uint32 SPELL_BOOK_SIZE      = 256;  // scribed spell slots
static const uint32 SPELL_MEMORY_SIZE    = 8;    // memorised spell gems
static const uint32 INVENTORY_SLOTS      = 30;   // main worn + general inv item IDs
static const uint32 CONTAINER_SLOTS      = 80;   // bag item IDs (10 bags × 8 slots)
static const uint32 CURSOR_BAG_SLOTS     = 10;   // cursor-bag item IDs
static const uint32 BANK_SLOTS           = 8;    // bank inventory item IDs
static const uint32 BANK_CONTAINER_SLOTS = 80;   // bank bag item IDs
static const uint32 MAX_CHARACTERS       = 10;   // character-select sends 10 slots
static const uint32 LANGUAGE_COUNT       = 24;
static const uint32 SKILL_COUNT          = 74;
static const uint32 GROUP_MEMBERS        = 6;
static const uint32 BIND_LOCATIONS       = 5;
static const uint32 PC_MAX_NAME_LENGTH   = 30;
static const uint32 PC_SURNAME_LENGTH    = 20;
static const uint32 NPC_MAX_NAME_LENGTH  = 30;

// Translate an EQEmu class id (Server/common/classes.h) to the Trilogy/EQClassic
// class id (EQClassic Common/Include/classes.h).  Player classes 1-15 are
// identical; NPC/special classes differ.  Most importantly Merchant
// (EQEmu 41 → Trilogy 32): the client uses the spawn's class byte to decide an
// NPC is a shoppable merchant on right-click, so without this the merchant
// window never opens.  Banker (40 → 16) and the GM "trainer" classes
// (EQEmu 20-34 → Trilogy 17-31) are mapped too.
static inline uint8 TranslateClassToTrilogy(uint8 c)
{
	switch (c) {
		case 41: return 32; // Merchant
		case 40: return 16; // Banker
		case 16: return 1;  // Berserker → Warrior (no Berserker in Velious-era)
		default:
			if (c >= 20 && c <= 34) return static_cast<uint8>(c - 3); // WarriorGM..BeastlordGM → 17..31
			if (c == 35)            return 17; // BerserkerGM → WarriorGM
			if (c >= 59 && c <= 71) return 32; // Discord/AltCurrency/etc. merchants → Merchant
			return c; // player classes 1-15 (and anything unmapped) pass through
	}
}

/*
** Compiler override to ensure byte-aligned structures.
** All Trilogy network structs must use #pragma pack(1).
*/
#pragma pack(1)

// -------------------------------------------------------------------------
// Login / world-handshake structs
// -------------------------------------------------------------------------

/*
** LoginInfo_Struct
** Opcode (world):  WS_SEND_LOGIN_INFO = 0x5818
** Direction:       client -> world server
** Source:          EQClassic LS/Login/login_structs.h :: LoginInfo_Struct
** Size:            40 bytes (+ variable tail containing logged-out server name)
**
** The 40-byte crypt[] block is DES-CBC encrypted with the Verant key (see
** trilogy_crypto.h).  Decrypted it yields:
**   char username[20]  at offset 0
**   char password[20]  at offset 20
*/
struct LoginInfo_Struct
{
/*000*/	int8	crypt[40];
/*040*/	int8	unknown[0];	// variable-length: null-terminated server-name tail
/*040*/
};

/*
** EnterWorld_Struct
** Opcode:  OP_ENTERWORLD = 0x0180
** Direction: client -> world server
** Source:  EQClassic Common/Include/eq_packet_structs.h :: EnterWorld_Struct
** Size:    PC_MAX_NAME_LENGTH (30) bytes
*/
struct EnterWorld_Struct
{
/*000*/	char	name[30];
/*030*/
};

/*
** SessionId_Struct
** Opcode (LS):  LS_SEND_SESSION_ID = 0x0400
** Direction:   login server -> client
** Source:      EQClassic LS/Login/login_structs.h :: SessionId_Struct
*/
struct SessionId_Struct
{
/*000*/	char	session_id[10];
/*010*/	char	unused[7];
/*017*/	int32	unknown;
/*021*/
};

// -------------------------------------------------------------------------
// Server-list structs (login server -> client)
// Source: EQClassic LS/Login/login_structs.h
// -------------------------------------------------------------------------

/*
** ServerList_Struct
** Opcode (LS):  LS_SEND_SERVERLIST = 0x4600
** Direction:   login server -> client
*/
struct ServerList_Struct
{
/*000*/	int8	numservers;
/*001*/	int8	unknown1;
/*002*/	int8	unknown2;
/*003*/	int8	showusercount;	// 0xFF=show user count, 0x00=show "UP"
/*004*/	uint8	data[0];		// variable: server entries
/*004*/
};

/*
** ServerListServerFlags_Struct
** Embedded within ServerList server entries.
*/
struct ServerListServerFlags_Struct
{
/*000*/	int8	greenname;
/*001*/	int32	usercount;
/*005*/	int8	unknown[8];
/*013*/
};

/*
** ServerListEndFlags_Struct
** Trailing flags in the ServerList packet.
*/
struct ServerListEndFlags_Struct
{
/*000*/	int32	admin;
/*004*/	int8	zeroes_a[8];
/*012*/	int8	kunark;
/*013*/	int8	velious;
/*014*/	int8	zeroes_b[11];
/*025*/
};

// -------------------------------------------------------------------------
// Character-select structs
// -------------------------------------------------------------------------

/*
** CharacterSelect_Struct
** Opcode:  OP_SendCharInfo = 0x4720  (WS_SEND_CHAR_INFO)
** Direction: world server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: CharacterSelect_Struct
** Size:    1248 bytes
*/
struct CharacterSelect_Struct
{
/*0000*/	char	name[10][30];		// character names (10 slots × 30 chars)
/*0300*/	int8	level[10];
/*0310*/	int8	class_[10];
/*0320*/	int16	race[10];
/*0340*/	char	zone[10][20];		// current zone short-names
/*0540*/	int8	gender[10];
/*0550*/	int8	face[10];
/*0560*/	int8	equip[10][9];		// helm/chest/arm/bracer/hand/leg/boot/wpn1/wpn2
/*0650*/	int8	unknown1[2];
/*0652*/	uint32	cs_colors[10][9];	// RRGGBBAA per slot
/*1012*/	int8	unknown2[20];
/*1032*/	int8	unknown3[216];
/*1248*/
};

/*
** CharWeapon_Struct
** Sent alongside SendCharInfo to show weapons on character select screen.
** Source:  EQClassic Common/Include/eq_packet_structs.h :: CharWeapon_Struct
*/
struct CharWeapon_Struct
{
	int16	righthand[10];
	int16	lefthand[10];
};

/*
** NameApproval_Struct
** Opcode:  OP_NAMEAPPROVAL = 0x8b20
** Direction: client -> world (request) / world -> client (response)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: NameApproval_Struct
*/
struct NameApproval_Struct
{
/*000*/	char	charname[30];	// proposed character name
/*030*/	uint8	unknown1;
/*031*/	uint8	unknown2;
/*032*/	uint8	race;
/*033*/	uint8	unknown3;
/*034*/	uint8	unknown4;
/*035*/	uint8	unknown5;
/*036*/	uint8	class_;
/*037*/	uint8	unknown6;
/*038*/
};

// -------------------------------------------------------------------------
// Zone-entry structs
// -------------------------------------------------------------------------

/*
** ClientZoneEntry_Struct
** Opcode:  OP_ZoneEntry = 0x2a20
** Direction: client -> zone server
** Source:  EQClassic QuagmireGhostCheck: strncpy(tmp, &app->pBuffer[4], 16)
**          EQMacEmuTrilogy structs: uint32 unknown00 + char char_name[32]
** Size:    34 bytes
*/
struct ClientZoneEntry_Struct
{
/*000*/	uint32	unknown00;	// 4-byte prefix before name
/*004*/	char	name[30];	// character name (null-terminated)
/*034*/
};

/*
** ZoneServerInfo_Struct
** Opcode:  OP_ZoneServerInfo = 0x0480
** Direction: world server -> client
** Source:  EQClassic World/Source/ClientNetwork.cpp :: SendZoneServerInfo()
** Size:    130 bytes (confirmed by APPLAYER(OP_ZoneServerInfo, 130))
**
** Layout from EQClassic SendZoneServerInfo:
**   strcpy(buf+0,  zs->GetCAddress());   // IP string
**   strcpy(buf+75, zone_name);           // zone short name
**   *(int16*)(buf+128) = ntohs(port);    // port in NETWORK byte order (big-endian)
**                                        // ntohs(host_port) on LE = htons(host_port) = byte-swap
**                                        // Client reads with ntohs() to recover host port value
*/
struct ZoneServerInfo_Struct
{
/*000*/	char	ip[75];		// dotted-decimal ASCII e.g. "127.0.0.1\0"
/*075*/	char	zone_name[53];	// zone short name, null-terminated
/*128*/	uint16	port;		// NETWORK byte order (big-endian); use htons(host_port) when setting
/*130*/
};

/*
** ServerZoneEntry_Struct
** Opcode:  OP_ZoneEntry = 0x2a20
** Direction: zone server -> client (and client -> zone server during zone-in)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: ServerZoneEntry_Struct
** Size:    312 bytes
**
** Server fills this after loading PlayerProfile and sends it to spawn the
** entering character in the zone.  Client sends a matching payload on connect
** (checksum[4] + name[30] at offset 4 in QuagmireGhostCheck).
** SetEQChecksum() writes CRC of bytes [4..311] into bytes [0..3].
*/
struct ServerZoneEntry_Struct
{
/*000*/	int8	checksum[4];
/*004*/	int8	sze_unknown1;
/*005*/	char	name[30];
/*035*/	char	zone[15];
/*050*/	int8	sze_unknown2[6];
/*056*/	float	y;
/*060*/	float	x;
/*064*/	float	z;
/*068*/	float	heading;
/*072*/	int8	sze_unknown3[76];
/*148*/	int16	guildeqid;
/*150*/	int8	sze_unknown3_78[3];
/*153*/	int8	sze_unknown_add[4];	// [3]=0 normal, 1=name as dead, 2=death on zone-in
/*157*/	int8	class_;
/*158*/	int16	race;
/*160*/	int8	gender;
/*161*/	int8	level;
/*162*/	int8	sze_unknown5[2];
/*164*/	int8	pvp;
/*165*/	int8	sze_unknown5_2_1[2];
/*167*/	int8	face;
/*168*/	int8	helmet;			// helmet material type
/*169*/	int8	sze_unknown5_2_2[6];
/*175*/	int8	left_hand;		// left weapon material
/*176*/	int8	right_hand;		// right weapon material
/*177*/	int8	sze_unknown5_2_3[3];
/*180*/	uint32	helmcolor;		// RGBA
/*184*/	int8	sze_unknown5_2_4[32];
/*216*/	int8	npc_armor_graphic;	// 0xFF=Player, 0=none, 1=leather, 2=chain, 3=plate
/*217*/	int8	sze_unknown5_2_5[19];
/*236*/	float	walkspeed;
/*240*/	float	runspeed;
/*244*/	int8	sze_unknown6[12];
/*256*/	int8	anon;
/*257*/	int8	sze_unknown6_13[23];
/*280*/	char	Surname[20];		// PC_SURNAME_LENGTH = 20
/*300*/	int8	sze_unknown_add2[2];
/*302*/	int16	deity;
/*304*/	int8	sze_unknown_add2_1[8];
/*312*/
};

/*
** NewZone_Struct
** Opcode:  OP_NewZone = 0x5b20
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: NewZone_Struct
** Size:    372 bytes (unknown_end[32] in EQClassic source is an artifact,
**          confirmed by the "// 372 bytes" comment above the struct there)
*/
struct NewZone_Struct
{
/*000*/	char	char_name[30];
/*030*/	char	zone_short_name[15];
/*045*/	int8	unknown045[5];
/*050*/	char	zone_long_name[180];
/*230*/	int8	zonetype;
/*231*/	int8	fog_red[4];
/*235*/	int8	fog_green[4];
/*239*/	int8	fog_blue[4];
/*243*/	int8	unknown243;
/*244*/	float	fog_minclip[4];
/*260*/	float	fog_maxclip[4];
/*276*/	float	gravity;			// 0.4f = normal EQ gravity; 0 = zero-g / moon-jump
/*280*/	int8	unknown280[50];
/*330*/	int8	sky;
/*331*/	float	unknown331;
/*335*/	int8	unknown335[9];
/*344*/	float	safe_x;
/*348*/	float	safe_y;
/*352*/	float	safe_z;
/*356*/	int8	unknown356[4];
/*360*/	float	underworld;
/*364*/	float	minclip;
/*368*/	float	maxclip;
/*372*/
};

// -------------------------------------------------------------------------
// Spawn / entity structs
// -------------------------------------------------------------------------

/*
** Spawn_Struct
** Opcode:  OP_ZoneSpawns = 0x6121 (array), OP_NewSpawn = 0x4921 (single)
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Spawn_Struct
** Size:    164 bytes (confirmed: Yeahlight in EQClassic source)
**
** NOTE on velocity bitfield: the deltaY/Z/X packed sint32 uses C bitfield
** packing.  Phase 4 encode/decode handlers MUST use explicit bit manipulation
** rather than relying on struct bitfield layout (compiler-dependent behaviour
** in packed structs).
*/
struct Spawn_Struct
{
/*000*/	float	size;
/*004*/	float	walkspeed;
/*008*/	float	runspeed;
/*012*/	int32	equipcolors[7];		// RGBA armour tints
/*040*/	int8	s_unknown1a[9];
/*049*/	int8	heading;
/*050*/	int8	deltaHeading;		// signed
/*051*/	int16	y_pos;				// signed fixed-point
/*053*/	int16	x_pos;				// signed fixed-point
/*055*/	int16	z_pos;				// signed fixed-point
/*057*/	int32	deltaY:10,			// velocity Y (signed 10-bit)
				spacer1:1,
				deltaZ:10,			// velocity Z (signed 10-bit)
				spacer2:1,
				deltaX:10;			// velocity X (signed 10-bit)
/*061*/	int8	s_unknown2[1];
/*062*/	int16	spawn_id;
/*064*/	int16	body_type;			// TBodyType
/*066*/	int16	pet_owner_id;
/*068*/	int16	cur_hp;				// signed
/*070*/	uint16	GuildID;
/*072*/	int8	race;
/*073*/	int8	NPC;				// 0=Player, 1=NPC, 2=PC Corpse, 3=NPC Corpse
/*074*/	int8	class_;
/*075*/	int8	gender;
/*076*/	int8	level;
/*077*/	int8	invis;
/*078*/	int8	unknown078;
/*079*/	int8	pvp;
/*080*/	int8	anim_type;
/*081*/	int8	light;
/*082*/	int8	anon;
/*083*/	int8	AFK;
/*084*/	int8	s_unknown5_2;
/*085*/	int8	LD;
/*086*/	int8	GM;
/*087*/	int8	s_unknown5_5;
/*088*/	int8	npc_armor_graphic;	// 0xFF=Player, 0=none, 1=leather, 2=chain, 3=plate
/*089*/	int8	npc_helm_graphic;
/*090*/	int8	s_unknown5_8;
/*091*/	int8	equipment[9];		// helm/chest/arm/bracer/hand/leg/boot/wpn1/wpn2
/*100*/	char	name[30];
/*130*/	char	Surname[20];
/*150*/	int8	guildrank;			// GUILDRANK: -1=Unknown, 0=Member, 1=Officer, 2=Leader
/*151*/	int8	unknown161;
/*152*/	int8	deity;
/*153*/	int8	unknown163[11];
/*164*/
};

/*
** NewSpawn_Struct
** Opcode:  OP_NewSpawn = 0x4921
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: NewSpawn_Struct
** Size:    168 bytes (4 header + 164 Spawn_Struct)
*/
struct NewSpawn_Struct
{
/*000*/	int32		ns_unknown1;
/*004*/	Spawn_Struct	spawn;
/*168*/
};

/*
** DeleteSpawn_Struct
** Opcode:  OP_DeleteSpawn = 0x2b20
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: DeleteSpawn_Struct
** Size:    6 bytes
*/
struct DeleteSpawn_Struct
{
/*000*/	int16	spawn_id;
/*002*/	int32	ds_unknown1;
/*006*/
};

/*
** SpawnAppearance_Struct
** Opcode:  OP_SpawnAppearance = 0xf520
** Direction: zone server -> client (and client -> server for self)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: SpawnAppearance_Struct
** Size:    12 bytes
*/
struct SpawnAppearance_Struct
{
/*000*/	int16	spawn_id;
/*002*/	int8	unknown1[2];
/*004*/	int16	type;				// TSpawnAppearanceType
/*006*/	int8	unknown2;
/*007*/	int8	unknown4;
/*008*/	int32	parameter;
/*012*/
};

/*
** Illusion_Struct
** Opcode:  OP_Illusion = 0x9120
** Direction: zone server -> client
** Source:  EQClassic Zone/Source/mob.cpp :: SendIllusionPacket
**          EQClassic Common/Include/eq_packet_structs.h :: Illusion_Struct
** Size:    72 bytes
**
** The client identifies the target entity by NAME (not spawn_id), reading the
** name field as a null-terminated string from offset 0.  EQClassic's own
** SendIllusionPacket uses strcpy (no length limit), so names longer than 15
** chars overflow into unknown016-028 — the client handles this gracefully.
** Do NOT use strncpy to fill name; use the raw-buffer helper to write the full
** null-terminated name so long-named NPCs (e.g. "Priest of Discord") match.
** jackbauer at offset 48 must always be 24 or the client ignores the packet.
** race/gender/texture/helm/face are int16.
*/
struct Illusion_Struct
{
/*000*/	char	name[16];		// name of entity to re-skin
/*016*/	int8	unknown016[4];
/*020*/	int8	unknown020[4];
/*024*/	int8	unknown024[4];
/*028*/	int8	unknown028[2];
/*030*/	char	target[16];		// same as name (self-cast illusion design)
/*046*/	int8	unknown046[2];
/*048*/	int16	jackbauer;		// always 24
/*050*/	int16	unknown050;
/*052*/	int16	unknown052;
/*054*/	int16	unknown054;
/*056*/	int8	unknown056[4];
/*060*/	int8	unknown060[2];
/*062*/	int16	race;
/*064*/	int16	gender;
/*066*/	int16	texture;
/*068*/	int16	helm;
/*070*/	int16	face;
/*072*/
};

/*
** SpawnPositionUpdate_Struct
** Used as element of SpawnPositionUpdates_Struct (OP_MobUpdate = 0xa120)
** and as the payload for OP_ClientUpdate = 0xf320.
** Source:  EQClassic Common/Include/eq_packet_structs.h :: SpawnPositionUpdate_Struct
** Size:    15 bytes
**
** NOTE: same velocity bitfield caveat as Spawn_Struct — use explicit bit ops.
*/
struct SpawnPositionUpdate_Struct
{
/*000*/	int16	spawn_id;
/*002*/	int8	anim_type;			// signed
/*003*/	int8	heading;
/*004*/	int8	delta_heading;		// signed
/*005*/	int16	y_pos;				// signed
/*007*/	int16	x_pos;				// signed
/*009*/	int16	z_pos;				// signed
/*011*/	int32	delta_y:10,
				spacer1:1,
				delta_z:10,
				spacer2:1,
				delta_x:10;
/*015*/
};

/*
** SpawnPositionUpdates_Struct
** Opcode:  OP_MobUpdate = 0xa120
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: SpawnPositionUpdates_Struct
*/
struct SpawnPositionUpdates_Struct
{
/*000*/	int32				num_updates;
/*004*/	SpawnPositionUpdate_Struct	spawn_update[0];
/*004*/
};

/*
** TimeOfDay_Struct
** Opcode:  OP_TimeOfDay = 0xf220
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: TimeOfDay_Struct
** Size:    6 bytes
*/
struct TimeOfDay_Struct
{
/*000*/	int8	hour;
/*001*/	int8	minute;
/*002*/	int8	day;
/*003*/	int8	month;
/*004*/	int16	year;
/*006*/
};

/*
** Stamina_Struct
** Opcode:  OP_Stamina = 0x5721
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Stamina_Struct
** Size:    8 bytes (6 fields + 2 padding per EQClassic "Length: 8 Bytes" comment)
*/
struct Stamina_Struct
{
/*000*/	int16	food;		// 6000=full, 0=starving
/*002*/	int16	water;		// 6000=full, 0=parched
/*004*/	int16	fatigue;	// 0=rested, 100=exhausted
/*006*/	int16	unknown006;
/*008*/
};

// -------------------------------------------------------------------------
// Player Profile (8104 bytes)
// -------------------------------------------------------------------------

/*
** SpellBuff_Struct  (10 bytes)
** Embedded in PlayerProfile_Struct::buffs[BUFF_COUNT].
** Source:  EQClassic Common/Include/PlayerProfile.h :: SpellBuff_Struct
*/
struct SpellBuff_Struct
{
/*000*/	int8	visable;		// 0=not visible, 1=visible+permanent, 2=visible+timed
/*001*/	int8	level;			// caster level
/*002*/	int8	bard_modifier;
/*003*/	int8	b_unknown3;
/*004*/	int16	spellid;
/*006*/	int32	duration;		// in ticks
/*010*/
};

/*
** ItemProperties_Struct  (10 bytes)
** Embedded in PlayerProfile_Struct inventory property arrays.
** Source:  EQClassic Common/Include/PlayerProfile.h :: ItemProperties_Struct
*/
struct ItemProperties_Struct
{
/*000*/	int8	unknown01[2];
/*002*/	int8	charges;		// signed: -1=unlimited
/*003*/	int8	unknown02[7];
/*010*/
};

/*
** PlayerProfile_Struct
** Opcode:  OP_PlayerProfile = 0x2d20
** Direction: zone server -> client
** Source:  EQClassic Common/Include/PlayerProfile.h :: PlayerProfile_Struct
** Size:    8104 bytes
**
** Buffer-over-read guard: when encoding, iterate ONLY up to the Trilogy
** constants (SPELL_BOOK_SIZE=256, BUFF_COUNT=15, etc.), never the larger
** EQEmu server-side maximums.
*/
struct PlayerProfile_Struct
{
/*0000*/	int8	checksum[4];
/*0004*/	char	name[30];
/*0034*/	char	Surname[20];
/*0054*/	int8	gender;
/*0055*/	int8	deity;
/*0056*/	int16	race;
/*0058*/	int8	class_;
/*0059*/	int8	pp_unknown3;
/*0060*/	int8	level;
/*0061*/	int8	pp_unknown4[3];
/*0064*/	int32	exp;
/*0068*/	int16	trainingpoints;
/*0070*/	int16	mana;
/*0072*/	int8	face;
/*0073*/	int8	unknown0073[47];
/*0120*/	int16	cur_hp;
/*0122*/	int8	pp_unknown7;
/*0123*/	int8	STR;
/*0124*/	int8	STA;
/*0125*/	int8	CHA;
/*0126*/	int8	DEX;
/*0127*/	int8	INT;
/*0128*/	int8	AGI;
/*0129*/	int8	WIS;
/*0130*/	int8	languages[24];
/*0154*/	int8	pp_unknown8_2[14];
/*0168*/	uint16	inventory[30];
/*0228*/	uint32	inventoryitemPointers[30];	// zero-fill: client-side pointers
/*0348*/	ItemProperties_Struct	invItemProprieties[30];
/*0648*/	SpellBuff_Struct	buffs[15];		// BUFF_COUNT=15, 150 bytes
/*0798*/	uint16	containerinv[80];
/*0958*/	uint16	cursorbaginventory[10];
/*0978*/	ItemProperties_Struct	bagItemProprieties[80];
/*1778*/	ItemProperties_Struct	cursorItemProprieties[10];
/*1878*/	int16	spell_book[256];			// SPELL_BOOK_SIZE=256
/*2390*/	int16	spell_memory[8];			// SPELL_MEMORY_SIZE=8
/*2406*/	int8	pp_unknown10[2];
/*2408*/	float	y;
/*2412*/	float	x;
/*2416*/	float	z;
/*2420*/	float	heading;
/*2424*/	char	current_zone[15];
/*2439*/	int8	pp_unknown13[21];
/*2460*/	int32	platinum;
/*2464*/	int32	gold;
/*2468*/	int32	silver;
/*2472*/	int32	copper;
/*2476*/	int32	platinum_bank;
/*2480*/	int32	gold_bank;
/*2484*/	int32	silver_bank;
/*2488*/	int32	copper_bank;
/*2492*/	int32	platinum_cursor;
/*2496*/	int32	gold_cursor;
/*2500*/	int32	silver_cursor;
/*2504*/	int32	copper_cursor;
/*2508*/	int8	skills[74];
/*2582*/	int8	unknown2582[34];
/*2616*/	int8	test_unknown;
/*2617*/	int8	unknown2617[127];
/*2744*/	int8	autosplit;
/*2745*/	int8	unknown2745[3];
/*2748*/	int8	pvpEnabled;
/*2749*/	int8	unknown2749[15];
/*2764*/	int8	gm;
/*2765*/	int8	unknown2765[23];
/*2788*/	int8	discplineAvailable;
/*2789*/	int8	unknown2789[23];
/*2812*/	int32	hungerlevel;
/*2816*/	int32	thirstlevel;
/*2820*/	int8	unknown2820[24];
/*2844*/	char	bind_point_zone[20];
/*2864*/	char	start_point_zone[4][20];
/*2944*/	ItemProperties_Struct	bankinvitemproperties[8];
/*3024*/	ItemProperties_Struct	bankbagitemproperties[80];
/*3824*/	int8	unknown3824[4];
/*3828*/	float	bind_location[3][5];		// [bind_idx][coord]: y=0,x=1,z=2 per EQClassic
/*3888*/	int8	unknown3888[24];
/*3912*/	int32	bankinvitemPointers[8];		// zero-fill: client-side pointers
/*3944*/	int8	unknown3944[12];
/*3956*/	int32	time1;
/*3960*/	int8	unknown3960[20];
/*3980*/	int16	bank_inv[8];
/*3996*/	int16	bank_cont_inv[80];
/*4156*/	int8	deity_wire;					// Deity displayed by the char sheet (raw EQEmu ID 140 or 201-216). Confirmed by capturing the CharCreate payload: client picks Innoruuk -> payload byte 4152 (= struct byte 4156) = 0xCE (206).
/*4157*/	int8	unknown4157;				// Padding byte adjacent to deity_wire.
/*4158*/	int16	guildid;
/*4160*/	int32	time2;
/*4164*/	int8	unknown4164[6];
/*4170*/	int8	fatigue;
/*4171*/	int8	unknown4171[2];
/*4173*/	int8	anon;
/*4174*/	int8	unknown4174;
/*4175*/	int8	guildrank;		// GUILDRANK: 0=Member, 1=Officer, 2=Leader
/*4176*/	int8	drunkeness;
/*4177*/	int8	eqbackground;
/*4178*/	int8	unknown4178;
/*4179*/	int8	unknown4179;
/*4180*/	uint32	spellSlotRefresh[8];
/*4212*/	int8	unknown4212[4];
/*4216*/	uint32	abilityCooldown;
/*4220*/	char	GroupMembers[6][48];
/*4508*/	int8	unknown4508[3592];
/*8100*/	int32	logtime;
/*8104*/
};

// -------------------------------------------------------------------------
// Combat / action structs
// -------------------------------------------------------------------------

/*
** Death_Struct
** Opcode:  OP_Death = 0x4a20
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Death_Struct
** Size:    20 bytes
*/
struct Death_Struct
{
/*000*/	int32	spawn_id;
/*004*/	int32	killer_id;
/*008*/	int32	corpseid;
/*012*/	int16	unknown12;
/*014*/	int8	attack_skill;
/*015*/	int8	unknown15;
/*016*/	int16	damage;
/*018*/	int16	unknown18;
/*020*/
};

/*
** SpawnHPUpdate_Struct
** Opcode:  OP_HPUpdate = 0xb220
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: SpawnHPUpdate_Struct
** Size:    12 bytes
*/
struct SpawnHPUpdate_Struct
{
/*000*/	int32	spawn_id;
/*004*/	int32	cur_hp;		// signed
/*008*/	int32	max_hp;		// signed
/*012*/
};

/*
** ManaChange_Struct
** Opcode:  OP_ManaChange = 0x7f21
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: ManaChange_Struct
** Size:    4 bytes
*/
struct ManaChange_Struct
{
/*000*/	uint16	new_mana;
/*002*/	uint16	spell_id;
/*004*/
};

/*
** Action_Struct
** Opcode:  OP_Action = 0x5820
** Direction: zone server -> client (combat actions and damage notifications)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Action_Struct
** Size:    28 bytes
**
** NOTE: EQEmu OP_Damage also encodes via this struct at the same Trilogy opcode.
*/
struct Action_Struct
{
/*000*/	int32	target;
/*004*/	int32	source;
/*008*/	int8	type;
/*009*/	int8	unknown005;
/*010*/	int16	spell;
/*012*/	int32	damage;		// signed
/*016*/	int8	unknown4[12];
/*028*/
};

/*
** Attack_Struct
** Opcode:  OP_Attack = 0x9f20
** Direction: client -> zone server (auto-attack initiation)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Attack_Struct
** Size:    12 bytes
*/
struct Attack_Struct
{
/*000*/	int32	spawn_id;
/*004*/	int8	type;
/*005*/	int8	a_unknown2[7];
/*012*/
};

// -------------------------------------------------------------------------
// Spell structs
// -------------------------------------------------------------------------

/*
** CastSpell_Struct
** Opcode:  OP_CastSpell = 0x7e21
** Direction: client -> zone server
** Source:  EQClassic Common/Include/eq_packet_structs.h :: CastSpell_Struct
** Size:    16 bytes
*/
struct CastSpell_Struct
{
/*000*/	uint16	slot;
/*002*/	uint16	spell_id;
/*004*/	int16	inventoryslot;	// 0xFFFF = normal cast, otherwise clicky item slot
/*006*/	int8	cs_unknown1[2];
/*008*/	uint32	target_id;
/*012*/	int32	cs_unknown2;
/*016*/
};

/*
** BeginCast_Struct
** Opcode:  OP_BeginCast = 0xa920
** Direction: zone server -> client (show cast bar), client -> server (cast started)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: BeginCast_Struct
** Size:    12 bytes
*/
struct BeginCast_Struct
{
/*000*/	int32	caster_id;
/*004*/	int32	spell_id;
/*008*/	int32	cast_time;	// in milliseconds
/*012*/
};

/*
** CastOn_Struct
** Opcode:  OP_CastOn = 0x4620
** Direction: zone server -> client (spell landed notification)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: CastOn_Struct
** Size:    36 bytes
*/
struct CastOn_Struct
{
/*000*/	int32	target_id;
/*004*/	int32	source_id;
/*008*/	int8	source_level;
/*009*/	int8	unknown1[4];
/*013*/	int8	unknown_zero1[7];
/*020*/	float	heading;
/*024*/	int8	unknown_zero2[4];
/*028*/	int32	action;
/*032*/	int16	spell_id;
/*034*/	int8	unknown2[2];
/*036*/
};

/*
** Buff_Struct
** Opcode:  OP_Buff = 0x3221
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Buff_Struct
** Size:    20 bytes
*/
struct Buff_Struct
{
/*000*/	uint32	target_id;
/*004*/	uint32	b_unknown1;
/*008*/	uint16	spell_id;
/*010*/	uint32	b_unknown2;
/*014*/	uint16	b_unknown3;
/*016*/	uint32	buff_slot;
/*020*/
};

/*
** MemorizeSpell_Struct
** Opcode:  OP_MemorizeSpell = 0x8221
** Direction: both (client memorize request, server confirmation)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: MemorizeSpell_Struct
** Size:    12 bytes
**
** scribing: 0=scribe to book, 1=memorize to gem, 3=gray-out gem (forget)
*/
struct MemorizeSpell_Struct
{
/*000*/	int32	slot;
/*004*/	int32	spell_id;
/*008*/	int32	scribing;
/*012*/
};

// -------------------------------------------------------------------------
// Chat
// -------------------------------------------------------------------------

/*
** ChannelMessage_Struct
** Opcode:  OP_ChannelMessage = 0x0721
** Direction: both
** Source:  EQClassic Common/Include/eq_packet_structs.h :: ChannelMessage_Struct
** Size:    68 bytes fixed + variable message tail
*/
struct ChannelMessage_Struct
{
/*000*/	char	targetname[32];
/*032*/	char	sender[23];
/*055*/	int8	cm_unknown2[9];
/*064*/	int16	language;
/*066*/	int16	chan_num;
/*068*/	int8	cm_unknown4[2];
/*070*/	char	message[0];		// variable-length null-terminated
/*070*/
};

// -------------------------------------------------------------------------
// Appearance / equipment
// -------------------------------------------------------------------------

/*
** WearChange_Struct
** Opcode:  OP_WearChange = 0x9220
** Direction: both
** Source:  EQClassic Common/Include/eq_packet_structs.h :: WearChange_Struct
** Size:    16 bytes
*/
struct WearChange_Struct
{
/*000*/	int32	spawn_id;
/*004*/	int8	wear_slot_id;
/*005*/	int8	slot_graphic;
/*006*/	int16	sub_op;
/*008*/	int32	color;
/*012*/	int8	wc_unknown3;
/*013*/	int8	flag;
/*014*/	int8	wc_unknown4[2];
/*016*/
};

// -------------------------------------------------------------------------
// Inventory
// -------------------------------------------------------------------------

/*
** ClassicItem_Struct  (292 bytes)
** Opcode:  OP_CharInventory = 0xf621 (bulk, deflated), OP_SummonedItem = 0x7821 (single)
** Direction: zone server -> client
** Source:  EQMacEmuTrilogy common/patches/trilogy_structs.h :: Item_Struct
** Size:    292 bytes
**
** Sent during zone-in (CONNECTING3 handshake) as a deflated array inside
** OP_CharInventory (0xf621).  Wire format:
**   uint8  item_count          [pBuffer+0]
**   uint8  _pad                [pBuffer+1] = 0
**   uint8  deflated_items[]    [pBuffer+2..] — zlib-deflated ClassicItem_Struct[]
*/
struct ClassicItem_Struct
{
/*0000*/	char	name[35];
/*0035*/	char	lore[60];
/*0095*/	char	idfile[6];
/*0101*/	uint16	flag;
/*0103*/	int8	unknown0103[22];
/*0125*/	uint8	weight;
/*0126*/	int8	norent;         // 1=normal, 0=no-rent
/*0127*/	int8	nodrop;         // 1=normal, 0=no-drop
/*0128*/	uint8	size;
/*0129*/	int8	itemclass;      // 0=common, 1=container, 2=book
/*0130*/	uint16	id;
/*0132*/	uint16	icon;
/*0134*/	int16	equipslot;      // EQ slot ID (0-21 worn, 22-29 general, 251-330 bag contents)
/*0136*/	uint32	slots;          // bitmask of equippable slots
/*0140*/	int32	price;
/*0144*/	float	cur_x;
/*0148*/	float	cur_y;
/*0152*/	float	cur_z;
/*0156*/	float	heading;
/*0160*/	uint32	inv_refnum;
/*0164*/	int16	log_;
/*0166*/	int16	loot_log;
/*0168*/	uint16	avatar_level;
/*0170*/	uint16	bottom_feed;
// Union at 0172: book_data (120 bytes) OR common (120 bytes) — total 120 bytes → struct ends at 292
union {
	struct {
/*0172*/	int8	book;
/*0173*/	int16	booktype;
/*0175*/	char	filename[15];
/*0190*/	uint8	buffer1[102];   // pad to 120 bytes
	} book_data;
	struct {
/*0172*/	int8	astr;
/*0173*/	int8	asta;
/*0174*/	int8	acha;
/*0175*/	int8	adex;
/*0176*/	int8	aint_;
/*0177*/	int8	aagi;
/*0178*/	int8	awis;
/*0179*/	int8	mr;
/*0180*/	int8	fr;
/*0181*/	int8	cr;
/*0182*/	int8	dr;
/*0183*/	int8	pr;
/*0184*/	int8	hp;
/*0185*/	int8	mana;
/*0186*/	int8	ac;
/*0187*/	uint8	stackable;      // 1=stackable, 0=not stackable
/*0188*/	int8	gmflag;         // 0=normal, -1=GM item
/*0189*/	uint8	light;
/*0190*/	uint8	delay;
/*0191*/	uint8	damage;
/*0192*/	int8	effecttype1;    // 0=combat proc, 1=click no-class, 2=latent/worn, 3=expendable, 4=click worn, 5=click w/class, -1=none
/*0193*/	uint8	range_;
/*0194*/	uint8	itemtype;       // weapon skill type
/*0195*/	int8	magic;
/*0196*/	int8	effectlevel1;
/*0197*/	uint8	material;
/*0198*/	uint8	unknown3f[2];
/*0200*/	uint32	color;
/*0204*/	uint16	faction;
/*0206*/	uint16	effect1;
/*0208*/	uint16	classes;
/*0210*/	uint8	unknown23[2];
		union {
			struct {
/*0212*/		uint16	races;
/*0214*/		uint16	unknown0214;
/*0216*/		int8	click_effect_type;
			} normal;
			struct {
/*0212*/		uint8	bagtype;
/*0213*/		uint8	bagslots;
/*0214*/		int8	isbagopen;
/*0215*/		int8	bagsize;
/*0216*/		uint8	bagwr;
			} container;
		};
/*0217*/	uint8	effectlevel2;
/*0218*/	int8	charges;
/*0219*/	int8	effecttype2;
/*0220*/	uint16	effect2;
/*0222*/	int8	unknown0282;    // must be 0xFF
/*0223*/	int8	unknown0283;    // must be 0xFF
/*0224*/	uint8	unknown0284[4];
/*0228*/	float	sellrate;
/*0232*/	uint32	casttime;
/*0236*/	uint8	unknown0296[16];
/*0252*/	uint16	skillmodtype;
/*0254*/	int16	skillmodvalue;
/*0256*/	int16	banedmgrace;
/*0258*/	int16	banedmgbody;
/*0260*/	uint8	banedmgamt;
/*0261*/	uint8	unknown0321[3];
/*0264*/	uint8	reclevel;
/*0265*/	uint8	recskill;
/*0266*/	uint16	procrate;
/*0268*/	uint8	elemdmgtype;
/*0269*/	uint8	elemdmgamt;
/*0270*/	uint16	factionmod1;
/*0272*/	uint16	factionmod2;
/*0274*/	uint16	factionmod3;
/*0276*/	uint16	factionmod4;
/*0278*/	uint16	factionamt1;
/*0280*/	uint16	factionamt2;
/*0282*/	uint16	factionamt3;
/*0284*/	uint16	factionamt4;
/*0286*/	uint16	void346;
/*0288*/	uint16	deity;
/*0290*/	uint16	unknown290;
/*0292*/
	} common;
};
};

static_assert(sizeof(ClassicItem_Struct) == 292,
	"Trilogy ClassicItem_Struct must be 292 bytes");

/*
** MoveItem_Struct
** Opcode:  OP_MoveItem = 0x2c21
** Direction: client -> zone server
** Source:  EQClassic Common/Include/eq_packet_structs.h :: MoveItem_Struct
** Size:    12 bytes
*/
struct MoveItem_Struct
{
/*000*/	uint32	from_slot;
/*004*/	uint32	to_slot;
/*008*/	uint32	number_in_stack;
/*012*/
};

// -------------------------------------------------------------------------
// Merchant / vendor structs
//   Source: EQClassic Common/Include/eq_packet_structs.h
//           + Zone/Source/client_process.cpp (ProcessOP_Shop*)
//   Opcodes: OP_ShopRequest 0x0b20, OP_ShopItem 0x0c20,
//            OP_ShopPlayerBuy 0x2720, OP_ShopPlayerSell 0x2820,
//            OP_ShopDelItem 0x3820, OP_ShopEnd 0x3720, OP_ShopEndConfirm 0x4521
// -------------------------------------------------------------------------

/*
** Merchant_Click_Struct
** Opcode:  OP_ShopRequest = 0x0b20
** Direction: bidirectional (client requests open; server replies open/close)
** Size:    16 bytes
**
** unknown[0] = action: 1 = open window, 0 = close (faction/busy/invis refusal)
** unknown[1] = 0x03 (constant set by EQClassic server)
** pricemultiplier = global buy markup; client shows buy = item.price * mult,
**                   sell = item.price / mult.  We send EQEmu's merchant `rate`.
*/
struct Merchant_Click_Struct
{
/*000*/	int32	entityid;        // merchant NPC entity (spawn) id
/*004*/	int32	playerid;        // requesting player's entity id
/*008*/	int8	unknown[4];
/*012*/	float	pricemultiplier;
/*016*/
};

/*
** Merchant_Purchase_Struct
** Opcode:  OP_ShopPlayerBuy = 0x2720 / OP_ShopPlayerSell = 0x2820
** Direction: bidirectional (request from client, confirm echoed back)
** Size:    20 bytes
**
** itemslot: on BUY  = merchant window slot (index assigned in Item_Shop_Struct.equipslot)
**           on SELL = player inventory wire slot
** itemcost: total transaction cost in copper (server fills on confirm)
*/
struct Merchant_Purchase_Struct
{
/*000*/	int32	npcid;           // merchant NPC entity id
/*004*/	int32	playerid;        // player's entity id
/*008*/	int16	itemslot;
/*010*/	int8	unknown001;
/*011*/	int8	unknown002;
/*012*/	int8	quantity;
/*013*/	int8	unknown003;
/*014*/	int8	unknown004;
/*015*/	int8	unknown005;
/*016*/	int32	itemcost;        // signed in EQClassic (sint32)
/*020*/
};

/*
** Merchant_DelItem_Struct
** Opcode:  OP_ShopDelItem = 0x3820
** Direction: server -> client (remove a depleted item from the open window)
** Size:    12 bytes
*/
struct Merchant_DelItem_Struct
{
/*000*/	int32	npcid;
/*004*/	int32	playerid;
/*008*/	int8	itemslot;
/*009*/	int8	unknown[3];
/*012*/
};

/*
** Item_Shop_Struct
** Opcode:  OP_ShopItem = 0x0c20
** Direction: server -> client (one item displayed in the merchant window)
** Size:    301 bytes (4 + 1 + 292 + 4)
**
** item.equipslot = the window slot the client echoes back on buy/sell
** item.price     = display cost (already divided by pricemultiplier so that
**                  client buy display = cost * pricemultiplier = real price)
*/
struct Item_Shop_Struct
{
/*000*/	uint32				merchantid;  // merchant LIST id (NPC MerchantType)
/*004*/	int8				itemtype;    // item->itemclass (0 common / 1 container / 2 book)
/*005*/	ClassicItem_Struct	item;
/*297*/	int8				iss_unknown001[4];
/*301*/
};

static_assert(sizeof(Merchant_Click_Struct)    == 16,  "Trilogy Merchant_Click_Struct must be 16 bytes");
static_assert(sizeof(Merchant_Purchase_Struct) == 20,  "Trilogy Merchant_Purchase_Struct must be 20 bytes");
static_assert(sizeof(Merchant_DelItem_Struct)  == 12,  "Trilogy Merchant_DelItem_Struct must be 12 bytes");
static_assert(sizeof(Item_Shop_Struct)         == 301, "Trilogy Item_Shop_Struct must be 301 bytes");

/*
** Consume_Struct
** Opcode:  OP_ConsumeFoodDrink = 0x5621
** Direction: client -> zone server
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Consume_Struct
** Size:    16 bytes
**
** auto_consumed: 0xffffffff=auto, 222=right-clicked
** Type: 0x01=food, 0x02=drink
*/
struct Consume_Struct
{
/*000*/	int32	slot;
/*004*/	int32	auto_consumed;
/*008*/	int8	c_unknown1[4];
/*012*/	int8	Type;
/*013*/	int8	unknown13[3];
/*016*/
};

// -------------------------------------------------------------------------
// Miscellaneous gameplay structs
// -------------------------------------------------------------------------

/*
** Consider_Struct
** Opcode:  OP_Consider = 0x3721
** Direction: client -> server (request), server -> client (response)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Consider_Struct
** Size:    28 bytes
*/
struct Consider_Struct
{
/*000*/	int32	playerid;
/*004*/	int32	targetid;
/*008*/	int32	faction;
/*012*/	int32	unknown_c[3];
/*024*/	int32	unworthy;
/*028*/
};

/*
** GroupUpdate_Struct
** Opcode:  OP_GroupUpdate = 0x2620
** Direction: zone server -> client
** Source:  EQMacEmuTrilogy/common/patches/trilogy_structs.h :: GroupUpdate_Struct
** Size:    228 bytes
**
** action: 0=join, 1=?, 2=?, 3=other leaves, 4=you leave
** Names are 32-byte NUL-terminated (PC_MAX_NAME_LENGTH on v29c).
** membername[] holds the OTHER five group members from your perspective
** (yourname is you, leader-or-not).
*/
struct GroupUpdate_Struct
{
/*000*/	char	yourname[32];
/*032*/	char	othername[32];		// in join/leave events, the member acted on
/*064*/	char	membername[5][32];
/*224*/	uint32	action;
/*228*/
};

/*
** GroupInvite_Struct
** Opcode:  OP_GroupInvite = 0x3e20 (popup) AND OP_GroupInvite2 = 0x4020
** Direction: BOTH (client sends to start; server pushes popup to invitee)
** Source:  EQMacEmuTrilogy trilogy_structs.h — trust the field sizes
**          (char[30] / char[30] / uint8[31]), NOT the offset comments
**          in that file which suggest 64/64/65 — those are decorative
**          and inconsistent with the actual field declarations.  The
**          v29c wire is 91 bytes (= 30 + 30 + 31), confirmed via
**          packet capture from a live invite attempt.
** Size:    91 bytes (30 + 30 + 31)
*/
struct GroupInvite_Struct
{
/*00*/	char	invitee_name[30];
/*30*/	char	inviter_name[30];
/*60*/	uint8	unknown[31];
/*91*/
};

/*
** GroupFollow_Struct
** Opcode:  OP_GroupFollow = 0x4220 (NOT 0x3d20 — EQMacEmuTrilogy is WRONG
**          here; the v29c client actually sends 0x4220, matching EQClassic
**          Common.  Opposite of OP_GroupInvite where EQMacEmu was right.)
** Direction: client -> server (the invitee accepts)
** Source:  Confirmed by packet capture 2026-06-18: 60-byte payload with
**          "Landar" at offset 0 and "Danalan" at offset 30.
** Size:    60 bytes
*/
struct GroupFollow_Struct
{
/*00*/	char	leader[30];		// inviter
/*30*/	char	invited[30];	// the one accepting
/*60*/
};

/*
** GroupInviteDecline_Struct
** Opcode:  OP_GroupCancelInvite = 0x4120
** Direction: BOTH (client declines; server echoes to inviter)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: GroupInviteDecline_Struct
**          (uses PC_MAX_NAME_LENGTH = 30, NOT 32)
** Size:    61 bytes — confirmed by live capture 2026-06-20 (matches v29c wire)
**
** Same name-width trap as GroupInvite_Struct (91B) and GroupFollow_Struct (60B):
** the name fields are [30], NOT [32].  Sending/expecting 65B drops the packet.
**
** action: 1=busy/considering another invite, 2=already grouped, 3=rejected
*/
struct GroupInviteDecline_Struct
{
/*00*/	char	leader[30];		// who invited
/*30*/	char	leaver[30];		// who's declining
/*60*/	uint8	action;
/*61*/
};

/*
** GroupDisband_Struct
** Opcode:  OP_GroupDisband = 0x4420
** Direction: client -> server (member leaves, kicks, or leader disbands)
** Source:  EQMacEmuTrilogy / EQClassic
** Size:    60 bytes
*/
struct GroupDisband_Struct
{
/*00*/	char	member[15];		// name (NUL-terminated, short — v29c only sends first 14 chars)
/*15*/	uint8	unknown1[41];
/*56*/	uint8	framecounter;	// increments by 32 each successive send by same client
/*57*/	uint8	unknown2[3];
/*60*/
};

/*
** ZoneChange_Struct
** Opcode:  OP_ZoneChange = 0xa320
** Direction: client -> zone server (zone-out request)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: ZoneChange_Struct
** Size:    68 bytes
*/
struct ZoneChange_Struct
{
/*000*/	char	char_name[32];		// PC_MAX_NAME_LENGTH+2 in EQClassic
/*032*/	char	zone_name[16];		// ZONE_SHORTNAME_LENGTH
/*048*/	int8	zc_unknown1[20];
/*068*/
};

/*
** ExpUpdate_Struct
** Opcode:  OP_ExpUpdate = 0x9921
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: ExpUpdate_Struct
** Size:    4 bytes
*/
struct ExpUpdate_Struct
{
/*000*/	uint32	exp;
/*004*/
};

/*
** LevelUpdate_Struct
** Opcode:  OP_LevelUpdate = 0x9821
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: LevelUpdate_Struct
** Size:    2 bytes
*/
struct LevelUpdate_Struct
{
/*000*/	int8	level;
/*001*/	int8	can_delevel;	// 0=no delevel, non-zero=allow delevel
/*002*/
};

/*
** SkillUpdate_Struct
** Opcode:  OP_SkillUpdate = 0x8921
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: SkillUpdate_Struct
** Size:    8 bytes
*/
struct SkillUpdate_Struct
{
/*000*/	uint8	skillId;
/*001*/	int8	unknown1[3];
/*004*/	uint8	value;
/*005*/	int8	unknown2[3];
/*008*/
};

/*
** ClassTrain_Struct
** Opcode:  OP_ClassTraining = 0x9c20 (bidirectional — client right-clicks GM trainer,
**          server replies with the same opcode/size to open the training window).
** Source:  EQClassic Common/Include/eq_packet_structs.h :: ClassTrain_Struct
** Size:    148 bytes
**
** highesttrain[i]    = max value to which the trainer can raise skill i (0..72).
**                      Value 0 hides the skill from the window.
** unknown[32]        = MUST contain non-zero bytes (EQClassic reference sets every
**                      byte to 1) or the trainer dialog never opens.  This is the
**                      "magic flag block" the client checks before drawing the UI.
** highesttrainLang[] = max value for each language (24 slots — Harakiri-era field;
**                      v29c clients render the language list when populated).
*/
struct ClassTrain_Struct
{
/*000*/	int32	npcid;
/*004*/	int32	playerid;
/*008*/	int8	highesttrain[73];
/*081*/	int8	unknown[32];          // must be non-zero or window won't open
/*113*/	int8	highesttrainLang[24];
/*137*/	int8	unknown2[11];
/*148*/
};

/*
** ClassSkillChange_Struct
** Opcode:  OP_ClassTrainSkill = 0x4021 (client -> zone)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: ClassSkillChange_Struct
** Size:    12 bytes
*/
struct ClassSkillChange_Struct
{
/*000*/	int32	npcid;
/*004*/	int32	skill_type;	// 0 = regular skill, 1 = language
/*008*/	int32	skill_id;
/*012*/
};

/*
** ClassTrainEnd_Struct
** Opcode:  OP_ClassEndTraining = 0x9d20 (client -> zone, when window is closed)
** Source:  EQClassic LS/common/eq_packet_structs.h :: GMTrainEnd_Struct
** Size:    4 bytes
*/
struct ClassTrainEnd_Struct
{
/*000*/	int16	npcid;
/*002*/	int16	unknown;
/*004*/
};

/*
** SetServerFilter_Struct
** Opcode:  OP_SetServerFilter = 0xff21
** Direction: client -> zone server
** Source:  EQClassic Common/Include/eq_packet_structs.h :: SetServerFilter_Struct
** Size:    60 bytes
*/
struct SetServerFilter_Struct
{
/*000*/	int8	unknown[60];
/*060*/
};

/*
** WhoAll_Struct
** Opcode:  OP_WhoAll = 0xf420
** Direction: client -> zone server
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Who_All_Struct
** Size:    76 bytes
*/
struct WhoAll_Struct
{
/*000*/	char	whom[32];
/*032*/	int16	wrace;		// 0xFFFF = no race filter
/*034*/	int16	wclass;		// 0xFFFF = no class filter
/*036*/	int16	firstlvl;	// 0xFFFF = no level filter
/*038*/	int16	secondlvl;
/*040*/	int16	gmlookup;	// 0xFFFF = not /who all gm
/*042*/	int8	unknown[34];
/*076*/
};

/*
** TradeSkillCombine_Struct
** Opcode:  OP_TradeSkillCombine = 0x0521
** Direction: client -> zone server
** Source:  EQClassic Common/Include/eq_packet_structs.h :: Combine_Struct
** Size:    32 bytes
*/
struct TradeSkillCombine_Struct
{
/*000*/	int8	worldobjecttype;	// non-zero = world tradeskill container
/*001*/	int8	unknown001;
/*002*/	int8	unknown002;
/*003*/	int8	unknown003;
/*004*/	int16	containerslot;		// slot of combine container (1000=world object)
/*006*/	int16	iteminslot[10];		// item IDs in container slots
/*026*/	int16	unknown005;
/*028*/	int16	unknown006;
/*030*/	int16	containerID;		// container item ID
/*032*/
};

/*
** TeleportPC_Struct
** Opcode:  OP_TeleportPC = 0x4d21
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: TeleportPC_Struct
** Size:    48 bytes
**
** The Trilogy client automatically decides whether to zone or intra-zone teleport
** based on whether zone[] matches the current zone short name.
** heading is sent *2 by the server; the client divides by 2.
** If zPos == 0, send 0.1f instead (EQClassic client bug with exact 0 z).
*/
struct TeleportPC_Struct
{
/*0000*/ char  zone[16];   // destination zone short name
/*0016*/ int8  unknown[16];
/*0032*/ float yPos;
/*0036*/ float xPos;
/*0040*/ float zPos;
/*0044*/ float heading;    // send heading*2; client divides by 2
/*0048*/
};

/*
** ClientTarget_Struct
** Opcode:  OP_ClientTarget = 0x6221
** Direction: client -> zone server
** Source:  EQClassic Common/Include/eq_packet_structs.h :: ClientTarget_Struct
** Size:    4 bytes
*/
struct ClientTarget_Struct
{
/*000*/	int16	new_target;		// entity ID of targeted mob/player
/*002*/	int16	ct_unknown1;
/*004*/
};

/*
** MoneyOnCorpse_Struct
** Opcode:  OP_MoneyOnCorpse = 0x5020
** Direction: zone server -> client
** Source:  EQClassic Common/Include/eq_packet_structs.h :: MoneyOnCorpse_Struct
** Size:    20 bytes
**
** Identical layout to EQEmu moneyOnCorpseStruct — pass-through.
*/
struct MoneyOnCorpse_Struct
{
/*000*/	uint8	response;		// 0=deny, 1=ok, 2=corpse empty, 3=too far
/*001*/	uint8	unknown1;
/*002*/	uint8	unknown2;
/*003*/	uint8	unknown3;
/*004*/	uint32	platinum;
/*008*/	uint32	gold;
/*012*/	uint32	silver;
/*016*/	uint32	copper;
/*020*/
};

/*
** LootingItem_Struct
** Opcode:  OP_LootItem = 0xa020
** Direction: client -> server (request), server -> client (echo)
** Source:  EQClassic Common/Include/eq_packet_structs.h :: LootingItem_Struct
** Size:    16 bytes
**
** Compatible with EQEmu LootingItem_Struct (same field order/sizes).
** slot_id is the corpse loot slot (starting at CORPSE_BEGIN=23).
*/
struct LootingItem_Struct
{
/*000*/	int32	lootee;			// entity ID of corpse
/*004*/	int32	looter;			// entity ID of looter (player)
/*008*/	int16	slot_id;		// corpse loot slot index
/*010*/	int8	unknown3[2];
/*012*/	int32	auto_loot;		// 0=manual, non-zero=auto
/*016*/
};

// -------------------------------------------------------------------------
// Restore structure packing to default
// -------------------------------------------------------------------------
#pragma pack()

// -------------------------------------------------------------------------
// Session signature collision check
//
// EQStreamIdentifier selects a patch by matching the first application-layer
// packet's (opcode, payload_size) pair.  These assertions confirm that the
// Trilogy signature sizes are distinct from Titanium's so the identifier
// cannot misclassify a connecting client.
//
//   World: Trilogy  LoginInfo_Struct  = 40  bytes
//          Titanium LoginInfo_Struct  = 488 bytes   → no collision
//   Zone:  Trilogy  ClientZoneEntry_Struct = 34 bytes (uint32 + name[30])
//          Titanium ClientZoneEntry_Struct = 68 bytes  → no collision
//   Note: Zone connections are handled by TrilogyZoneServer (EQNetwork/OnUnknownPacket);
//         the Daybreak patch signature is a dead-code fallback.
// -------------------------------------------------------------------------
static_assert(sizeof(LoginInfo_Struct)       == 40,
	"Trilogy LoginInfo_Struct must be 40 bytes "
	"(world-stream discriminator vs Titanium 488 bytes)");

static_assert(sizeof(ClientZoneEntry_Struct) == 34,
	"Trilogy ClientZoneEntry_Struct must be 34 bytes "
	"(uint32 unknown00 + char name[30])");


static_assert(sizeof(CharacterSelect_Struct) == 1248,
	"Trilogy CharacterSelect_Struct must be 1248 bytes");

// -------------------------------------------------------------------------
// Phase 3: Key struct size assertions
// -------------------------------------------------------------------------
static_assert(sizeof(SpellBuff_Struct)        == 10,
	"Trilogy SpellBuff_Struct must be 10 bytes");

static_assert(sizeof(ItemProperties_Struct)   == 10,
	"Trilogy ItemProperties_Struct must be 10 bytes");

static_assert(sizeof(PlayerProfile_Struct)    == 8104,
	"Trilogy PlayerProfile_Struct must be 8104 bytes");

static_assert(sizeof(ZoneServerInfo_Struct)   == 130,
	"Trilogy ZoneServerInfo_Struct must be 130 bytes");

static_assert(sizeof(ServerZoneEntry_Struct)  == 312,
	"Trilogy ServerZoneEntry_Struct must be 312 bytes");

static_assert(sizeof(NewZone_Struct)          == 372,
	"Trilogy NewZone_Struct must be 372 bytes");

static_assert(sizeof(Spawn_Struct)            == 164,
	"Trilogy Spawn_Struct must be 164 bytes");

static_assert(sizeof(NewSpawn_Struct)         == 168,
	"Trilogy NewSpawn_Struct must be 168 bytes");

static_assert(sizeof(DeleteSpawn_Struct)      ==   6,
	"Trilogy DeleteSpawn_Struct must be 6 bytes");

static_assert(sizeof(SpawnPositionUpdate_Struct) == 15,
	"Trilogy SpawnPositionUpdate_Struct must be 15 bytes");

static_assert(sizeof(Illusion_Struct) == 72,
	"Trilogy Illusion_Struct must be 72 bytes (EQClassic Zone/Common format)");

static_assert(sizeof(ClientTarget_Struct)   ==   4,
	"Trilogy ClientTarget_Struct must be 4 bytes");

static_assert(sizeof(MoneyOnCorpse_Struct)  ==  20,
	"Trilogy MoneyOnCorpse_Struct must be 20 bytes");

static_assert(sizeof(LootingItem_Struct)    ==  16,
	"Trilogy LootingItem_Struct must be 16 bytes");

	} /*structs*/
} /*Trilogy*/

#endif /*COMMON_TRILOGY_STRUCTS_H*/
