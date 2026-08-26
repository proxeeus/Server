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
#include "trilogy_client.h"
#include "trilogy_zone.h"
#include "../common/eq_constants.h"
#include "entity.h"
#include "doors.h"
#include "object.h"
#include "npc.h"
#include "corpse.h"
#include "water_map.h"
#include "mob_movement_manager.h"
#include "../common/eq_packet_structs.h"
#include "../common/patches/trilogy_structs.h"
#include "../common/item_instance.h"
#include "../common/item_data.h"
#include "../common/strings.h"
#include "../common/crc32.h"
#include "../common/eqemu_logsys.h"
#include "../common/emu_versions.h"
#include "../common/races.h"
#include "../common/classes.h"
#include "../common/textures.h"
#include "string_ids.h"
#include "../common/zone_store.h"
#include "../common/spdat.h"

#ifndef _WINDOWS
#  include <arpa/inet.h>
#  include <netinet/in.h>
#else
#  include <winsock2.h>
#endif

#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

extern EntityList entity_list;

// ============================================================
// EncryptSpawnPacket — cipher for single OP_NewSpawn (0x4921).
// Same stream as EncryptZoneSpawnPacket in trilogy_zone.cpp but
// without the initial dword-swap (which is ZoneSpawns-specific).
// 'size' must be a multiple of 4.
// ============================================================
static void EncryptSpawnPacket(uint8_t* buf, uint32_t size)
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
// TrilogyStream
// ============================================================

TrilogyStream::TrilogyStream(const std::string& addr, uint16 port)
	: m_addr(addr), m_port(port)
{
#ifdef _WINDOWS
	m_ip = inet_addr(addr.c_str());
#else
	struct in_addr ia{};
	m_ip = (inet_aton(addr.c_str(), &ia) != 0) ? ia.s_addr : 0;
#endif
}

void TrilogyStream::FastQueuePacket(EQApplicationPacket** p, bool ack_req)
{
	// FastQueuePacket takes ownership and deletes *p.
	safe_delete(*p);
}

uint16 TrilogyStream::GetRemotePort() const
{
	return htons(m_port); // Client stores ntohs(eqs->GetRemotePort()), so return htons.
}

std::string TrilogyStream::Describe() const
{
	return std::string("TrilogyStream(") + m_addr + ":" + std::to_string(m_port) + ")";
}

// ============================================================
// TrilogyClient constructor
// ============================================================

TrilogyClient::TrilogyClient(
	TrilogyZoneServer* tzs,
	uint64_t           session_key,
	uint16_t           player_spawn_id,
	uint32_t           char_id,
	uint32_t           acct_id,
	const char*        acct_name,
	const char*        char_name,
	uint16_t           race,
	uint8_t            class_,
	uint8_t            gender,
	uint8_t            level,
	float              x,
	float              y,
	float              z,
	float              heading,
	const std::string& ip_addr,
	uint16_t           port)
: Client(new TrilogyStream(ip_addr, port))
, m_tzs(tzs)
, m_session_key(session_key)
, m_player_spawn_id(player_spawn_id)
{
	// Set private Client fields that bypass the normal zone-entry handshake.
	InitTrilogyFields(char_id, acct_id, acct_name, char_name);

	// Mob-level race/class/gender so GetRace()/GetClass()/GetGender() return correct values.
	ChangeRace(race);
	SetClass(class_);
	ChangeGender(gender);
	Mob::SetLevel(level); // bypass Client::SetLevel which fires quest events on an uninitialized client

	// Client::SetBase* mirrors values into m_pp so FillSpawnStruct reads them correctly.
	SetBaseRace(race);
	SetBaseClass(class_);
	SetBaseGender(gender);
	GetPP().level  = level;
	SetDeity(GetPP().deity); // use DB-loaded value; SetDeity sets both m_pp.deity and Mob::deity

	// Set initial world position without broadcasting (entity not yet in entity_list).
	SetPosition(x, y, z);
	SetHeading(heading);

	// Mirror position into m_pp so SaveCharacterData writes the correct location on
	// disconnect (m_pp.x/y/z default to 0 otherwise, placing the character at origin).
	GetPP().x       = x;
	GetPP().y       = y;
	GetPP().z       = z;
	GetPP().heading = heading;

	// Initialize HP/mana from DB-loaded m_pp values. The normal EQStream zone-entry path in
	// client_packet.cpp calls SetHP(m_pp.cur_hp) after CalcBonuses(), but Trilogy clients bypass
	// that path entirely. CalcBonuses() must come after Mob::SetLevel() so GetLevel()/GetSTA()
	// return correct values for CalcBaseHP().
	CalcBonuses();
	if (GetPP().cur_hp <= 0)
		GetPP().cur_hp = GetMaxHP();
	SetHP(GetPP().cur_hp);
	Mob::SetMana(GetPP().mana);

	LogInfo("[TrilogyClient] Created: char='{}' id={} race={} class={} level={} pos=({:.1f},{:.1f},{:.1f})",
	        char_name, char_id, race, static_cast<int>(class_), static_cast<int>(level), x, y, z);
}

// ============================================================
// CheckLoreConflict — DB-authoritative override (see header comment)
//
// Trilogy direct-DB ops (buy/sell/bank/HandleMoveItem) bypass m_inv, so the
// base Client::CheckLoreConflict (which queries m_inv) is unreliable for
// Trilogy clients after the zone-in load.  We query the `inventory` table
// directly using the same rules as EQ::ItemData::CheckLoreConflict:
//   • LoreFlag must be set and LoreGroup != 0 on the candidate.
//   • LoreGroup == -1 → conflict if an item with the same `itemid` already exists.
//   • LoreGroup  >  0 → conflict if any owned item has that same loregroup.
// Shared-bank slots (DB 2500-2999) are excluded to mirror the base behaviour;
// Trilogy never uses shared bank but staying consistent keeps the rule simple.
// ============================================================

bool TrilogyClient::CheckLoreConflict(const EQ::ItemData* item)
{
	if (!item)             return false;
	if (!item->LoreFlag)   return false;
	if (item->LoreGroup == 0) return false;

	std::string q;
	if (item->LoreGroup == -1) {
		// Standard "Lore Item" — any owned copy of the same item id collides.
		q = fmt::format(
		    "SELECT 1 FROM `inventory` "
		    "WHERE `charid`={} AND `itemid`={} "
		    "AND (`slotid` < 2500 OR `slotid` >= 3000) LIMIT 1",
		    CharacterID(), item->ID);
	} else {
		// Lore group — any owned item sharing that group collides.
		q = fmt::format(
		    "SELECT 1 FROM `inventory` i JOIN `items` it ON i.`itemid` = it.`id` "
		    "WHERE i.`charid`={} AND it.`loregroup`={} "
		    "AND (i.`slotid` < 2500 OR i.`slotid` >= 3000) LIMIT 1",
		    CharacterID(), item->LoreGroup);
	}

	auto r = database.QueryDatabase(q);
	return r.Success() && r.RowCount() > 0;
}

// ============================================================
// PickSummonTargetSlot — pick a visible destination for #summonitem so
// items never land in the invisible cursor queue (DB 8000-8010) that v29c
// can't render.  See trilogy_client.h for the return-value contract.
//
// Kept parallel to FindFreeTrilogyInvSlot in trilogy_zone.cpp (shop-buy
// path) — same layout knowledge (general = DB 23-30, bag content base =
// 251 + (general-23) * 10), so if that helper changes, mirror it here.
// ============================================================
int TrilogyClient::PickSummonTargetSlot() const
{
	bool cursor_busy   = false;
	bool occ_gen[31]   = {}; // DB 23..30 occupied
	int  bagslots[31]  = {}; // container capacity at general DB 23..30 (0 = not a container)
	bool occ_cont[331] = {}; // bag-content DB 251..330 occupied

	auto r = database.QueryDatabase(fmt::format(
	    "SELECT i.`slotid`, it.`itemclass`, it.`bagslots` FROM `inventory` i "
	    "LEFT JOIN `items` it ON i.`itemid` = it.`id` "
	    "WHERE i.`charid`={} AND (i.`slotid`=33 OR i.`slotid` BETWEEN 23 AND 30 "
	    "OR i.`slotid` BETWEEN 251 AND 330)",
	    CharacterID()));
	if (r.Success())
		for (auto row = r.begin(); row != r.end(); ++row) {
			const int sl        = Strings::ToInt(row[0]);
			const int itemclass = row[1] ? Strings::ToInt(row[1]) : 0;
			const int bs        = row[2] ? Strings::ToInt(row[2]) : 0;
			if (sl == EQ::invslot::slotCursor) {
				cursor_busy = true;
			} else if (sl >= 23 && sl <= 30) {
				occ_gen[sl] = true;
				if (itemclass == 1 && bs > 0) bagslots[sl] = bs;
			} else if (sl >= 251 && sl <= 330) {
				occ_cont[sl] = true;
			}
		}

	if (!cursor_busy) return EQ::invslot::slotCursor;

	for (int sl = 23; sl <= 30; ++sl)
		if (!occ_gen[sl]) return sl;

	for (int G = 23; G <= 30; ++G) {
		if (bagslots[G] <= 0) continue;
		const int base = 251 + (G - 23) * 10;
		const int n    = bagslots[G] > 10 ? 10 : bagslots[G];
		for (int j = 0; j < n; ++j)
			if (base + j <= 330 && !occ_cont[base + j]) return base + j;
	}

	return -1;
}

// ============================================================
// PurgeStaleCursorRowsForItem — see trilogy_client.h for the contract.
//
// Trade staging on Trilogy is metadata-only (HandleTradeMoveItem doesn't
// touch DB or m_inv), so a trade closed WITHOUT OP_CancelTrade leaves the
// cursor row stranded in DB.  The client-side cursor is empty; the server's
// CheckLoreConflict sees the row and blocks every future summon of that id.
// A GM using `#si <same_id>` is asking for a fresh copy — deleting the
// stale rows first is the intended semantic.  Also pops the m_inv cursor
// stack of any entries with this item_id to keep in-memory consistent.
// ============================================================
int TrilogyClient::PurgeStaleCursorRowsForItem(uint32 item_id)
{
	if (item_id == 0) return 0;

	auto r = database.QueryDatabase(fmt::format(
	    "DELETE FROM `inventory` WHERE `charid`={} AND `itemid`={} "
	    "AND (`slotid`=33 OR (`slotid` BETWEEN 8000 AND 8010))",
	    CharacterID(), item_id));
	const int deleted = r.Success() ? static_cast<int>(r.RowsAffected()) : 0;

	// Drain m_inv cursor of any entries whose item id matches — the stack
	// order isn't preserved by PopItem-and-repush, but we don't care: the
	// caller (command_summonitem) is about to create a fresh instance.
	auto& inv = GetInv();
	while (auto* top = inv.GetItem(EQ::invslot::slotCursor)) {
		if (!top->GetItem() || top->GetItem()->ID != item_id) break;
		if (auto* popped = inv.PopItem(EQ::invslot::slotCursor)) safe_delete(popped);
	}

	if (deleted > 0) {
		LogInfo("[TrilogyClient] PurgeStaleCursorRowsForItem char={} item={} "
		        "deleted {} orphan cursor row(s)",
		        CharacterID(), item_id, deleted);
	}
	return deleted;
}

// ============================================================
// CalcManaRegen — classic 1999 EQ formula (override)
//
// EQClassic LS/zone/client_process.cpp L6643-6669 is authoritative:
//   if (sitting && level > 0) medding = true;       // always true for live players
//   if (medding):
//     if (cur + skill/10 + level*3/4 + 4 < max/2):  // below half mana
//       regen = skill/10 + (level - level/4) + 4
//     else:                                         // approaching cap
//       regen = level + 6
//   else if (!sitting):
//     regen = 2
//
// Bards historically did not med. Mediate skillups are handled by the base
// DoManaRegen() (which calls CheckIncreaseSkill before SetMana) so we don't
// duplicate that here — we just return the per-tick delta.
//
// AreaManaRegen and item/spell mana-regen bonuses are intentionally left out:
// the user wants the classic feel, and those modifiers didn't exist in 1999.
// We do honor IsStarved() (no regen while starved) since that's a basic gate.
// ============================================================

int64 TrilogyClient::CalcManaRegen(bool bCombat)
{
	if (IsStarved()) return 0;
	if (GetClass() == Class::Bard) return 0;

	const int level = static_cast<int>(GetLevel());

	if (!IsSitting()) {
		return 2;
	}

	const int skill = static_cast<int>(GetSkill(EQ::skills::SkillMeditate));
	const int high_regen = (skill / 10) + (level - (level / 4)) + 4;
	const int low_regen  = level + 6;

	// Classic: if applying the full regen would still keep us under 50% mana,
	// use the full formula; otherwise fall back to (level + 6).
	if ((static_cast<int>(GetMana()) + high_regen) < (static_cast<int>(GetMaxMana()) / 2)) {
		return high_regen;
	}
	return low_regen;
}

// ============================================================
// GetEXPForLevel — EQClassic v29c race×class formula
//
// Mirrors EQClassic\Zone\Source\client.cpp:759-857.  The v29c eqgame.exe
// computes the XP-bar fill internally using this same hardcoded table; the
// server-side level threshold has to match or the bar visually fills before
// the server-side ding fires.  Applied unconditionally so users don't need to
// flip Character:UseOld{Class,Race}ExpPenalties (those EQEmu rules approximate
// the same numbers but slightly diverge for Rogues).
//
// Formula for level <31: (level-1)^3 × class_modifier × race_modifier
// Levels 31+ multiply by an additional level-band modifier (1.1..3.1).
// ============================================================
uint32 TrilogyClient::GetEXPForLevel(uint16 check_level)
{
	if (check_level < 2) return 0;

	// hum     bar     eru     elf     hie     def     hef     dwa     tro     ogr     hal    gno     iks     vah
	static constexpr float race_mods[14] = {
		100.0f, 105.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 120.0f, 115.0f, 95.0f, 100.0f, 120.0f, 120.0f
	};
	// war   cle    pal    ran    shd    dru    mnk    brd    rog    shm    nec    wiz    mag    enc
	static constexpr float class_mods[14] = {
		9.0f, 10.0f, 14.0f, 14.0f, 14.0f, 10.0f, 12.0f, 14.0f, 9.05f, 10.0f, 11.0f, 11.0f, 11.0f, 11.0f
	};

	uint8 base_race = GetBaseRace();
	int   race_idx;
	switch (base_race) {
		case IKSAR:   race_idx = 13 - 1; break; // EQClassic remap → index 12
		case VAHSHIR: race_idx = 14 - 1; break; // index 13
		default:      race_idx = static_cast<int>(base_race) - 1; break;
	}

	uint8 cls = GetClass();
	int   class_idx = static_cast<int>(cls) - 1;

	if (race_idx < 0 || race_idx >= 14 || class_idx < 0 || class_idx >= 14) {
		// Invalid race/class for the v29c table (e.g. Beastlord/Berserker).
		// Fall back to the EQEmu base formula so SetEXP doesn't get 0xFFFFFFFF
		// and abort the entire AddEXP path.
		return Client::GetEXPForLevel(check_level);
	}

	const uint32 lvm1 = static_cast<uint32>(check_level - 1);
	const float  cube = static_cast<float>(lvm1 * lvm1 * lvm1);
	const uint32 calc = static_cast<uint32>(cube * class_mods[class_idx] * race_mods[race_idx]);

	if (check_level < 31) return calc;

	float band =
		(check_level < 36) ? 1.1f :
		(check_level < 41) ? 1.2f :
		(check_level < 46) ? 1.3f :
		(check_level < 52) ? 1.4f :
		(check_level < 53) ? 1.5f :
		(check_level < 54) ? 1.6f :
		(check_level < 55) ? 1.7f :
		(check_level < 56) ? 1.9f :
		(check_level < 57) ? 2.1f :
		(check_level < 58) ? 2.3f :
		(check_level < 59) ? 2.5f :
		(check_level < 60) ? 2.7f :
		(check_level < 61) ? 3.0f :
		                     3.1f;

	return static_cast<uint32>(calc * band);
}

// ============================================================
// QueuePacket / FastQueuePacket — intercept all outgoing packets
// ============================================================

void TrilogyClient::QueuePacket(const EQApplicationPacket* app,
                                bool               ack_req,
                                CLIENT_CONN_STATUS filter_status,
                                eqFilterType       filter)
{
	if (!app) return;
	TranslateAndSend(app);
}

void TrilogyClient::FastQueuePacket(EQApplicationPacket** app,
                                    bool               ack_req,
                                    CLIENT_CONN_STATUS filter_status)
{
	if (!app || !*app) return;
	TranslateAndSend(*app);
	safe_delete(*app);
}

static const char* TrilogySystemStringTemplate(uint32_t string_id);

// ============================================================
// Combat-event token bucket (see m_pending_combat_q in trilogy_client.h)
//
// Rate: 60 tokens/sec sustained, 120 burst.  Applies to OP_SpecialMesg
// (0x8021), OP_Attack (0x9F20), and OP_Action (0x5820).  Sized to
// accommodate ~all 70 bots in a raid swinging simultaneously.
//
// Callers use QueueTextPacket(opcode, data, size) for any combat opcode.
// Fast path: queue empty AND token available → send immediately (no latency).
// Slow path: enqueue; DrainPendingText() flushes as tokens refill.
//
// Two overload defenses (both added 2026-08-07 after the 70-bot raid case):
//   1. Per-source dedup on OP_Attack + zero-damage OP_Action at enqueue —
//      a bot's newer swing replaces its older queued swing rather than
//      piling up.  Damage packets (OP_Action with damage != 0) are unique
//      event data and are NEVER deduped.
//   2. Stale-drop at drain — packets older than kMaxCombatQueueAgeMs are
//      discarded silently.  Under raid overload, wire traffic carries the
//      newest events instead of a 25-second stale backlog.  Directly fixes
//      the "combat text keeps arriving after the target is dead" symptom.
//
// Subsumes the earlier OP_SpecialMesg-only burst protection (the `^spells`
// case: 9 chat lines in <50 ms): a 120-token bucket absorbs that without
// queueing, so the user sees the list instantly.
// ============================================================

bool TrilogyClient::TryAcquireCombatToken()
{
	const uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	if (m_combat_tokens_last_ms != 0) {
		double elapsed_s = static_cast<double>(now_ms - m_combat_tokens_last_ms) / 1000.0;
		m_combat_tokens = std::min(
			static_cast<double>(kCombatBucketCapacity),
			m_combat_tokens + elapsed_s * kCombatBucketRefill);
	}
	m_combat_tokens_last_ms = now_ms;

	if (m_combat_tokens >= 1.0) {
		m_combat_tokens -= 1.0;
		return true;
	}
	return false;
}

void TrilogyClient::EnqueueCombatPacket(uint16_t opcode, const uint8_t* data, uint32_t size)
{
	const uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	// Classify: extract source_id and decide if this packet is eligible for
	// per-source dedup.  Under raid overload, one bot may fire OP_Attack
	// faster than the bucket drains — replacing the older queued entry
	// with the newest one keeps the animation stream current instead of
	// playing stale swings after the fight has ended.
	//
	// Wire structs:
	//   Attack_Struct (0x9F20): int32 spawn_id @ 0, int8 type @ 4       — 12 B
	//   Action_Struct (0x5820): int32 target @ 0, int32 source @ 4,
	//                           int8 type @ 8, int16 spell @ 10,
	//                           int32 damage @ 12                        — 28 B
	uint32_t source_id  = 0;
	bool     supersedes = false;
	if (opcode == 0x9f20 && size >= 4) {
		source_id  = *reinterpret_cast<const uint32_t*>(data);
		supersedes = true;
	}
	else if (opcode == 0x5820 && size >= 16) {
		source_id             = *reinterpret_cast<const uint32_t*>(data + 4);
		const int32_t damage  = *reinterpret_cast<const int32_t*>(data + 12);
		// Damage packets are unique (miss/hit values, spell landings) —
		// never dedup.  Zero-damage OP_Action is a pure animation trigger
		// (spell hit anim, cast complete effect) and IS superseded.
		supersedes = (damage == 0);
	}
	// OP_SpecialMesg (0x8021) chat: each line may carry different text —
	// never dedup.

	if (supersedes && source_id != 0) {
		for (auto& p : m_pending_combat_q) {
			if (p.opcode == opcode && p.source_id == source_id) {
				p.payload.assign(data, data + size);
				p.enqueued_ms = now_ms;
				return;
			}
		}
	}

	if (m_pending_combat_q.size() >= kMaxPendingCombat) {
		// Soft cap: drop oldest.  Hitting this means we're far over rate
		// budget for many seconds; losing leading chatter is the least-bad
		// option to keep latency bounded.
		m_pending_combat_q.pop_front();
	}
	PendingCombatPacket p;
	p.opcode      = opcode;
	p.payload.assign(data, data + size);
	p.enqueued_ms = now_ms;
	p.source_id   = source_id;
	p.supersedes  = supersedes;
	m_pending_combat_q.push_back(std::move(p));
}

void TrilogyClient::QueueTextPacket(uint16_t opcode, const uint8_t* data, uint32_t size)
{
	// Fast path: nothing queued AND token available → send immediately so
	// short bursts (combat hit, brief chat) don't get artificial latency.
	if (m_pending_combat_q.empty() && TryAcquireCombatToken()) {
		m_tzs->SendToSession(m_session_key, opcode, data, size);
		return;
	}
	// Else queue — DrainPendingText() will send it as soon as a token is
	// available, preserving FIFO order across the chat / animation stream.
	EnqueueCombatPacket(opcode, data, size);
}

void TrilogyClient::DrainPendingText()
{
	if (m_pending_combat_q.empty()) return;

	const uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	// Stale-drop pass: any packet older than kMaxCombatQueueAgeMs is
	// discarded without spending a token or hitting the wire.  This is the
	// primary defense against the "combat text keeps arriving after the
	// target is dead" symptom on Trilogy raid clients — under sustained
	// overload we prefer losing old events to burning wire bandwidth on
	// them.  Entries are FIFO, so we can pop from the front until we hit
	// a fresh one.
	while (!m_pending_combat_q.empty() &&
	       now_ms - m_pending_combat_q.front().enqueued_ms > kMaxCombatQueueAgeMs) {
		m_pending_combat_q.pop_front();
	}

	// Refill + spend tokens against remaining queued combat events.
	// Sending multiple per Tick is fine — the bucket is capped at burst 120.
	while (!m_pending_combat_q.empty() && TryAcquireCombatToken()) {
		auto& p = m_pending_combat_q.front();
		m_tzs->SendToSession(m_session_key, p.opcode,
		                     p.payload.data(),
		                     static_cast<uint32_t>(p.payload.size()));
		m_pending_combat_q.pop_front();
	}
}

bool TrilogyClient::IsCombatEventCloseToObserver(uint16_t source_id, uint16_t target_id) const
{
	const float my_x = GetX();
	const float my_y = GetY();

	auto within = [&](uint16_t id) -> bool {
		if (id == 0) return false;
		Mob* m = entity_list.GetMob(id);
		if (!m) return false;
		float dx = m->GetX() - my_x;
		float dy = m->GetY() - my_y;
		return (dx * dx + dy * dy) <= kCombatVisibilityRadiusSq;
	};
	return within(source_id) || within(target_id);
}

// ============================================================
// TranslateAndSend — dispatch by EQEmu internal opcode
// ============================================================

void TrilogyClient::TranslateAndSend(const EQApplicationPacket* app)
{
	if (!app) return;

	switch (app->GetOpcode()) {
	case OP_NewSpawn:
		HandleNewSpawn(app);
		break;
	case OP_DeleteSpawn:
		HandleDeleteSpawn(app);
		break;
	case OP_ClientUpdate:
		HandleClientUpdate(app);
		break;
	case OP_TimeOfDay:
		// Internal TimeOfDay_Struct has uint32 year; Trilogy wire wants int16 year.
		// Re-pack into the 6-byte Trilogy struct before forwarding.
		if (app->size >= sizeof(::TimeOfDay_Struct)) {
			const auto* tod_in = reinterpret_cast<const ::TimeOfDay_Struct*>(app->pBuffer);
			Trilogy::structs::TimeOfDay_Struct tod_out{};
			tod_out.hour   = static_cast<int8_t>(tod_in->hour);
			tod_out.minute = static_cast<int8_t>(tod_in->minute);
			tod_out.day    = static_cast<int8_t>(tod_in->day);
			tod_out.month  = static_cast<int8_t>(tod_in->month);
			tod_out.year   = static_cast<int16_t>(tod_in->year);
			m_tzs->SendToSession(m_session_key, 0xf220,
			                     reinterpret_cast<const uint8_t*>(&tod_out),
			                     static_cast<uint32_t>(sizeof(tod_out)));
		}
		break;
	case OP_Weather:
		// Wire format is already correct (Zone::weatherSend writes raw bytes
		// matching the Trilogy 8-byte layout); forward as-is.
		if (app->size == 8)
			m_tzs->SendToSession(m_session_key, 0x3621, app->pBuffer, 8);
		break;
	case OP_ChannelMessage:
		HandleOutgoingChannelMessage(app);
		break;
	case OP_SpecialMesg:
		HandleOutgoingSpecialMesg(app);
		break;
	case OP_FormattedMessage:
		HandleOutgoingFormattedMessage(app);
		break;
	case OP_SimpleMessage:
		HandleOutgoingSimpleMessage(app);
		break;
	case OP_Sound: {
		// quest::ding()/e.other:Ding() → Client::SendSound(), and Client::QuestReward()
		// also emits OP_Sound.  Map to the classic quest "fanfare" trumpet:
		// OP_QuestCompletedMoney (0x8020) with a zeroed 36-byte struct (no money here —
		// any coin reward is reflected by the money reconciliation in
		// TrilogyZoneServer::Tick).  Mirrors EQClassic SendClientQuestCompletedFanfare().
		uint8_t fanfare[36] = {};
		m_tzs->SendToSession(m_session_key, 0x8020, fanfare, sizeof(fanfare));
		break;
	}
	case OP_Illusion:
		HandleIllusion(app);
		break;
	case OP_Surname: {
		// Accept-echo for the sender only.  Client::Handle_OP_Surname built a
		// 100 B EQEmu Surname_Struct with unknown0064=1; repack into the 56 B
		// Trilogy Surname_Struct (name[16] + s_unknown1[20]=0x01 + Surname[20])
		// so the v29c client marks the request as accepted.  The GMSurname
		// broadcast below is what actually refreshes nameplates.
		if (app->size >= sizeof(::Surname_Struct)) {
			const auto* es = reinterpret_cast<const ::Surname_Struct*>(app->pBuffer);
			Trilogy::structs::Surname_Struct ts{};
			strn0cpy(ts.name, es->name, sizeof(ts.name));
			memset(ts.s_unknown1, 1, sizeof(ts.s_unknown1));
			strn0cpy(ts.Surname, es->lastname, sizeof(ts.Surname));
			m_tzs->SendToSession(m_session_key, 0xc421,
			                     reinterpret_cast<const uint8_t*>(&ts),
			                     static_cast<uint32_t>(sizeof(ts)));
		}
		break;
	}
	case OP_GMLastName: {
		// Broadcast surname change to nearby Trilogy players so their
		// nameplates refresh.  EQEmu's GMLastName_Struct is 200 B (three
		// 64-char name fields + uint16[4]); repack into the 94 B Trilogy
		// GMSurname_Struct with unknown[0]=1 to trigger the client update.
		// Sender is included in the broadcast (ChangeLastName calls
		// entity_list.QueueClients with ignore_sender=false).
		if (app->size >= sizeof(::GMLastName_Struct)) {
			const auto* gmln = reinterpret_cast<const ::GMLastName_Struct*>(app->pBuffer);
			Trilogy::structs::GMSurname_Struct ts{};
			strn0cpy(ts.name,    gmln->name,     sizeof(ts.name));
			strn0cpy(ts.gmname,  gmln->gmname,   sizeof(ts.gmname));
			strn0cpy(ts.Surname, gmln->lastname, sizeof(ts.Surname));
			// EQClassic ProcessOP_Surname (client_process.cpp:2196) memsets both
			// bytes to 1 — that is the "display the new surname" signal.
			memset(ts.unknown, 1, sizeof(ts.unknown));
			m_tzs->SendToSession(m_session_key, 0x6e21,
			                     reinterpret_cast<const uint8_t*>(&ts),
			                     static_cast<uint32_t>(sizeof(ts)));
		}
		break;
	}
	case OP_WearChange:
		HandleOutgoingWearChange(app);
		break;
	case OP_SpawnAppearance:
		HandleOutgoingSpawnAppearance(app);
		break;
	case OP_Animation:
		HandleAnimation(app);
		break;
	case OP_BeginCast:
		HandleBeginCast(app);
		break;
	case OP_Action:
		HandleAction(app);
		break;
	case OP_Damage:
		HandleDamage(app);
		break;
	case OP_Stun:
		if (app->size >= sizeof(::Stun_Struct)) {
			m_tzs->SendToSession(m_session_key, 0x5b21,
			                     app->pBuffer,
			                     static_cast<uint32_t>(sizeof(::Stun_Struct)));
		}
		break;
	case OP_InterruptCast:
	{
		if (app->size < sizeof(InterruptCast_Struct)) break;
		auto* ic = (InterruptCast_Struct*)app->pBuffer;
		const char* tmpl = TrilogySystemStringTemplate(ic->messageid);
		if (!tmpl) break;

		std::string text(tmpl);
		// _OTHER variants carry the caster name after the 8-byte header
		if (app->size > sizeof(InterruptCast_Struct)) {
			std::string name(ic->message,
			                 strnlen(ic->message, app->size - sizeof(InterruptCast_Struct)));
			for (size_t pos; (pos = text.find("%1")) != std::string::npos; )
				text.replace(pos, 2, name);
		}

		// Trilogy wire: int16 spawnid + int16 pad(0) + char[] text + null
		uint32_t out_size = 4 + static_cast<uint32_t>(text.size()) + 1;
		auto* out = new uint8_t[out_size]();
		*reinterpret_cast<int16_t*>(out)     = static_cast<int16_t>(TranslateId(ic->spawnid));
		*reinterpret_cast<int16_t*>(out + 2) = 0;
		memcpy(out + 4, text.data(), text.size());
		out[out_size - 1] = 0;
		m_tzs->SendToSession(m_session_key, 0xd321, out, out_size);
		delete[] out;
		break;
	}
	case OP_ManaChange:
		HandleManaChange(app);
		break;
	case OP_HPUpdate:
		HandleHPUpdate(app);
		break;
	case OP_MobHealth:
		HandleMobHealth(app);
		break;
	case OP_MemorizeSpell:
		HandleMemorizeSpellOut(app);
		break;
	case OP_Buff:
		HandleBuff(app);
		break;
	case OP_Death:
		HandleDeath(app);
		break;
	case OP_BecomeCorpse:
		HandleBecomeCorpse(app);
		break;
	case OP_Consider:
		HandleOutgoingConsider(app);
		break;
	case OP_ExpUpdate:
		HandleExpUpdate(app);
		break;
	case OP_LevelUpdate:
		HandleLevelUpdate(app);
		break;
	case OP_SkillUpdate: {
		if (app->size >= sizeof(::SkillUpdate_Struct)) {
			const auto* emu = reinterpret_cast<const ::SkillUpdate_Struct*>(app->pBuffer);
			Trilogy::structs::SkillUpdate_Struct sk{};
			sk.skillId = static_cast<uint8_t>(emu->skillId);
			sk.value   = static_cast<uint8_t>(emu->value);
			m_tzs->SendToSession(m_session_key, 0x8921,
			                     reinterpret_cast<const uint8_t*>(&sk),
			                     static_cast<uint32_t>(sizeof(sk)));
		}
		break;
	}
	case OP_MoneyOnCorpse:
		HandleMoneyOnCorpse(app);
		break;
	case OP_LootComplete:
		if (m_pending_loot_echo) {
			// Error path (lore conflict, corpse locked, etc.) — EQClassic sends only
			// the loot echo and returns; it does NOT send LootComplete (4421).
			// Sending 4421 here freezes the client.  Flush the echo only and let the
			// client keep the loot window open; the player closes it via Escape which
			// sends EndLootRequest → EndLoot → another OP_LootComplete (no pending
			// echo at that point) → we send 4421 and close the window cleanly.
			FlushPendingLootEcho();
		} else {
			// Normal end-of-loot (EndLootRequest → EndLoot path, or success path where
			// the echo was already flushed by HandleItemPacket) — close the window.
			m_tzs->SendToSession(m_session_key, 0x4421, nullptr, 0);
		}
		break;
	case OP_LootRequest:
		// Server echoes the 4-byte corpse ID back to the client.
		if (app->size >= 4)
			m_tzs->SendToSession(m_session_key, 0x4e20, app->pBuffer, 4);
		break;
	case OP_LootItem:
		HandleOutgoingLootItem(app);
		break;
	case OP_ItemPacket:
		HandleItemPacket(app);
		break;
	case OP_GroundSpawn:
		HandleGroundSpawn(app);
		break;
	case OP_MoveDoor:
		HandleMoveDoor(app);
		break;
	case OP_ShopRequest:
		HandleOutgoingShopRequest(app);
		break;
	case OP_ReadBook:
		HandleOutgoingReadBook(app);
		break;
	case OP_RezzRequest:
		HandleOutgoingRezzRequest(app);
		break;
	case OP_GroupInvite:
		// Popup invite emitted by Handle_OP_GroupInvite2 → GroupInvite_Struct.
		HandleOutgoingGroupInvite(app);
		break;
	case OP_GroupFollow:
		// Engine doesn't emit OP_GroupFollow outbound to clients in the normal
		// path (Follow is client→server), but include for completeness if
		// quest or world-cross-zone code ever wraps it back.
		HandleOutgoingGroupFollow(app);
		break;
	case OP_GroupCancelInvite:
		// Echoed to the inviter when the invitee declines.
		HandleOutgoingGroupCancelInvite(app);
		break;
	case OP_GroupDisband:
		// Engine never actually pushes OP_GroupDisband outbound — all leave
		// notifications come through OP_GroupUpdate. Kept for safety.
		HandleOutgoingGroupDisband(app);
		break;
	case OP_GroupUpdate:
		// Carries either GroupJoin_Struct (single-member event) or
		// GroupUpdate_Struct/GroupUpdate2_Struct (full roster bulk) — the
		// handler disambiguates by size + action field.
		HandleOutgoingGroupUpdate(app);
		break;
	case OP_ClickObject:
		// Remove a ground item from the client's view (pickup despawn broadcast).
		// EQClassic uses the same ClickObject_Struct layout; opcode 0x3620 = OP_PickupItem.
		if (app->size >= sizeof(::ClickObject_Struct))
			m_tzs->SendToSession(m_session_key, 0x3620,
			                     app->pBuffer,
			                     static_cast<uint32_t>(sizeof(::ClickObject_Struct)));
		break;
	case OP_ClickObjectAction:
		// Object::HandleClick → SetTradeskillObject() emits this when a player
		// clicks a world tradeskill container (forge, oven, brew barrel, etc).
		// EQEmu's 92-byte ClickObjectAction_Struct collapses to v29c's 22-byte
		// ClickObjectAck_Struct on wire 0xd720 (OP_CraftingStation).  Per-slot
		// item contents follow via OP_ItemPacket(ItemPacketWorldContainer) →
		// OP_StationItem 0xfb20 (handled in HandleItemPacket).
		if (app->size >= sizeof(::ClickObjectAction_Struct)) {
			const auto* in = reinterpret_cast<const ::ClickObjectAction_Struct*>(app->pBuffer);
			Trilogy::structs::ClickObjectAck_Struct out{};
			// Match EQClassic Zone/Source/object.cpp:320-328 byte-for-byte: memset
			// to zero, then write ONLY player_id, open, type, slot, icon_nr.  In
			// particular bytes 4-7 ("drop_id" in modern EQEmu) stay zero — EQClassic
			// never populates them on this packet, and v29c may be parsing nonzero
			// bytes there as a different opcode shape.
			//
			// CRITICAL: translate the EQEmu server-side entity ID to the wire
			// spawn ID v29c knows the player by (m_player_spawn_id).  The
			// v29c client only opens the station UI when the packet's
			// player_id matches its own self-ID — sending the raw EQEmu GetID()
			// makes v29c think the packet is for some other player and ignore it.
			out.player_id  = TranslateId(in->player_id);
			out.open       = static_cast<uint8_t>(in->open);
			out.type       = static_cast<uint8_t>(in->type);
			out.slot       = 0x0a; // always 10 slots
			out.icon_nr    = static_cast<uint16_t>(in->icon);
			LogInfo("[TRILOGY-TS] OP_ClickObjectAction → 0xd720 player_id={} drop_id={} open={} type={} icon={} name='{}' struct_size={}",
			        in->player_id, in->drop_id, in->open, in->type, in->icon,
			        in->object_name,
			        static_cast<unsigned>(sizeof(out)));
			// Hex dump so we can verify the wire layout against EQClassic emission.
			const auto* raw = reinterpret_cast<const uint8_t*>(&out);
			std::string hex;
			for (size_t i = 0; i < sizeof(out); ++i)
				hex += fmt::format(" {:02x}", raw[i]);
			LogInfo("[TRILOGY-TS] 0xd720 HEX:{}", hex);
			m_tzs->SendToSession(m_session_key, 0xd720,
			                     reinterpret_cast<const uint8_t*>(&out),
			                     static_cast<uint32_t>(sizeof(out)));
		}
		break;
	case OP_ClientReady:
		// Object::HandleClick emits OP_ClientReady (0-byte) right before the per-
		// slot item packets to tell modern clients the world-container UI is now
		// listening.  v29c has no equivalent — the client opens its station UI on
		// receipt of OP_CraftingStation (0xd720) and then accepts OP_StationItem
		// (0xfb20) directly.  Drop silently.
		break;
	case OP_ClearObject:
		// Object::HandleCombine emits OP_ClearObject after a world-container
		// combine to wipe the client's bag-content display.  v29c uses a 0-byte
		// OP_CleanStation 0x0522 for the same purpose (see EQClassic's
		// Client::DeleteItemInStation in Zone/Source/client.cpp).
		LogInfo("[TRILOGY-TS] OP_ClearObject → 0x0522 (OP_CleanStation)");
		m_tzs->SendToSession(m_session_key, 0x0522, nullptr, 0);
		break;
	case OP_TradeSkillCombine:
		// Object::HandleCombine emits a 0-byte OP_TradeSkillCombine on every
		// error/success path as an ack.  EQClassic instead re-queues the
		// original 32-byte Combine_Struct verbatim.  We send the echo from
		// TrilogyZoneServer::OnDatagram (right after Handle_OP_TradeSkillCombine
		// returns) where the original payload is still in scope, so drop the
		// empty version here — v29c rejects empty bodies on this opcode.
		break;
	case OP_DeleteItem:
	case OP_DeleteCharge:
	case OP_MoveItem: {
		// Server-initiated inventory delete (consumed bait, broken pole, bandage
		// used, etc.).  Modern EQEmu emits three distinct opcodes — all carry
		// the same 12-byte DeleteItem_Struct { from_slot; to_slot; number_in_stack }
		// — that the v29c client doesn't understand.  EQClassic has no
		// OP_DeleteItem at all; the client expects OP_MoveItem (0x2c21) with
		// to_slot=0xFFFFFFFF, same pattern HandleMemorizeSpell uses on the scribe
		// path (trilogy_zone.cpp:5930-5936).
		//
		// Semantics distinction we forward:
		//   OP_DeleteItem   (modern: decrement stack by 1, sent N times by caller)
		//     → wire OP_MoveItem(to=0xFFFFFFFF, number_in_stack=1)
		//   OP_DeleteCharge (modern: spend one clicky charge)
		//     → wire OP_MoveItem(to=0xFFFFFFFF, number_in_stack=1)
		//   OP_MoveItem with to=0xFFFFFFFF (modern: slot fully emptied)
		//     → wire OP_MoveItem(to=0xFFFFFFFF, number_in_stack=0)
		//   OP_MoveItem with any other to_slot
		//     → server-initiated rearrange; not used by the skill bridges in
		//       scope right now, so drop quietly until a use case appears.
		if (app->size < sizeof(::DeleteItem_Struct)) break;
		const auto* del = reinterpret_cast<const ::DeleteItem_Struct*>(app->pBuffer);
		if (app->GetOpcode() == OP_MoveItem && del->to_slot != 0xFFFFFFFFu) break;

		// Skip ammo + range slots.  The v29c client manages projectile-weapon
		// consumption locally on fire — arrows in slotAmmo (archery) AND
		// throwing weapons in slotRange (throwing).  EQClassic
		// Zone/Source/client_process.cpp:3028-3043 shows the server only
		// updates pp.inventory[21] for persistence, never sending a delete
		// packet to the client.  Forwarding our delete here causes the v29c
		// client to refuse the operation with "failed to move item in client
		// application" because the local stack was already decremented when
		// the projectile was fired.  Server-side m_inv + DB still update via
		// DeleteItemInInventory, so the next zone-in reflects reality.
		if (del->from_slot == static_cast<uint32_t>(EQ::invslot::slotAmmo) ||
		    del->from_slot == static_cast<uint32_t>(EQ::invslot::slotRange)) break;

		// Reverse slot translation: modern EQEmu RoF2 → v29c wire.  Mirrors the
		// existing forward map (TrilogyWireSlotToEmuSlot) used inbound, and the
		// HandleItemPacket inventory delivery shifts used outbound.
		auto emu_to_wire = [](uint32_t emu) -> uint32_t {
			if (emu == static_cast<uint32_t>(EQ::invslot::slotCursor)) return 0u;
			if (emu >= 22 && emu <= 30)  return emu - 1;  // general inventory shift
			if (emu >= 251 && emu <= 340) return emu - 1; // bag content shift
			return emu;                                   // equipment 1-20 / primary 13 / etc.
		};

		Trilogy::structs::MoveItem_Struct mv{};
		mv.from_slot       = emu_to_wire(del->from_slot);
		mv.to_slot         = 0xFFFFFFFFu;
		mv.number_in_stack =
		    (app->GetOpcode() == OP_MoveItem) ? 0u  // empty the slot
		                                      : 1u; // decrement one charge / stack
		m_tzs->SendToSession(m_session_key, 0x2c21,
		                     reinterpret_cast<const uint8_t*>(&mv),
		                     static_cast<uint32_t>(sizeof(mv)));
		break;
	}
	case OP_SomeItemPacketMaybe: {
		// Archery / throwing projectile animation.  Modern Mob::SendItemAnimation
		// (special_attacks.cpp:1706-1750) builds a 136-byte Arrow_Struct on
		// OP_SomeItemPacketMaybe carrying source xyz, source/target ids, item
		// model name + id, velocity, launch angle, tilt, arc and the in-use
		// skill.  V29c uses OP_SpawnProjectile (0x4520) with a 116-byte
		// SpawnProjectile_Struct (EQClassic eq_packet_structs.h:2178-2210)
		// whose extra physics fields (burst velocity / yaw / pitch / spawn
		// behaviour / projectile type / source animation / texture) describe
		// the same physical event with classic-EQ-engine terminology.
		//
		// Without this translation the player sees damage land on the mob
		// but no firing animation and no arrow flight — the v29c client got
		// nothing to render because the modern opcode dropped at default.
		if (app->size < sizeof(::Arrow_Struct)) break;
		const auto* a = reinterpret_cast<const ::Arrow_Struct*>(app->pBuffer);

#pragma pack(push, 1)
		struct VSpawnProjectile {
			int32_t always1;
			int32_t always0;
			int32_t test1;
			float   y;
			float   x;
			float   z;
			float   heading;
			float   tilt;
			float   velocity;
			float   burstVelocity;
			float   burstHorizontal;
			float   burstVertical;
			float   yaw;
			float   pitch;
			float   arc;
			int8_t  test5[4];
			int32_t sourceID;
			int32_t targetID;
			int16_t test6;
			int16_t test7;
			int32_t spellID;
			int8_t  lightSource;
			int8_t  test9;
			int8_t  spawnBehavior;
			int8_t  projectileType;
			int8_t  sourceAnimation;
			char    texture[16];
			char    spacer[15];
		};
#pragma pack(pop)
		static_assert(sizeof(VSpawnProjectile) == 116, "Trilogy SpawnProjectile_Struct must be 116 bytes");

		VSpawnProjectile p{};
		p.always1         = 1;
		p.always0         = 0;
		p.test1           = 0;
		p.y               = a->src_y;
		p.x               = a->src_x;
		p.z               = a->src_z;
		p.heading         = a->launch_angle;
		p.tilt            = a->tilt;
		p.velocity        = a->velocity;
		p.burstVelocity   = 0.0f;
		p.burstHorizontal = 0.0f;
		p.burstVertical   = 0.0f;
		p.yaw             = 0.0f;
		p.pitch           = 0.0f;
		p.arc             = a->arc;
		p.sourceID        = static_cast<int32_t>(TranslateId(a->source_id));
		p.targetID        = static_cast<int32_t>(TranslateId(a->target_id));
		p.spellID         = 0; // physical arrow, not spell bolt
		p.lightSource     = 0;
		p.spawnBehavior   = 1; // enable attack animation + projectile spawn
		// Projectile type per EQClassic comment: 0x11 = Arrow (default for
		// SkillArchery / SkillThrowing), 0x09 = spell bolt (not applicable here).
		p.projectileType  = 0x11;
		// Source animation — DoAnim(9) is EQClassic's archery shoot pose.
		p.sourceAnimation = 9;
		strncpy(p.texture, a->model_name, sizeof(p.texture) - 1);

		m_tzs->SendToSession(m_session_key, 0x4520,
		                     reinterpret_cast<const uint8_t*>(&p),
		                     static_cast<uint32_t>(sizeof(p)));
		break;
	}
	case OP_Bind_Wound: {
		// Bind Wound response echo.  Modern BindWound() pushes a sequence of
		// OP_Bind_Wound packets via QueuePacket with `type` carrying the
		// bind-state code (3=Unlock Interface, 0=ack, 1=Complete, 4=Target Died,
		// 5=Target Left Zone, 6=Target Moved Away, 7=You Moved).  The v29c
		// client expects these same codes on opcode 0x9320 with the same
		// 8-byte BindWound_Struct layout (int16 to; int16 unk; int16 type;
		// int16 unk) — direct byte-for-byte forward works.  Without this case
		// the proxy default drops the completion signal so the user sees no
		// HP heal feedback (HP itself still updates server-side via
		// SendHPUpdate, which the proxy already translates).
		if (app->size < sizeof(::BindWound_Struct)) break;
		// Legacy v29c BindWound_Struct (EQClassic/Common/Include/
		// eq_packet_structs.h:529-535) is byte-identical to the modern struct,
		// so memcpy the buffer; then translate `to` from modern GetID() back
		// to v29c player_spawn_id for self-bind so the client matches the
		// response to the right spawn.
#pragma pack(push, 1)
		struct VBindWoundStruct {
			int16_t to;
			int16_t unknown2;
			int16_t type;
			int16_t unknown6;
		};
#pragma pack(pop)
		static_assert(sizeof(VBindWoundStruct) == 8, "Trilogy BindWound_Struct must be 8 bytes");
		VBindWoundStruct out{};
		memcpy(&out, app->pBuffer, sizeof(::BindWound_Struct));
		out.to = static_cast<int16_t>(TranslateId(static_cast<uint32_t>(static_cast<uint16_t>(out.to))));
		m_tzs->SendToSession(m_session_key, 0x9320,
		                     reinterpret_cast<const uint8_t*>(&out),
		                     static_cast<uint32_t>(sizeof(out)));
		break;
	}
	case OP_Begging: {
		// Begging result echo.  Modern BeggingResponse_Struct (20 bytes) carries
		// only Result (0=fail, 1=plat, 2=gold, 3=silver, 4=copper) at offset 12
		// and Amount at offset 16.  V29c expects the legacy Beg_Struct (18 bytes)
		// echoed back on opcode 0x2521 with the same success/coins semantics, so
		// the client can display the success/fail line and tick its local coin
		// counter (the proxy's TrilogyZoneServer::Tick reconciliation still
		// pushes the coin delta separately within ~1s as a safety net).
		// Source: EQClassic Zone/Beg.cpp:152-155 — server modifies the inbound
		// packet in place and echoes it back via QueuePacket.
		if (app->size < sizeof(::BeggingResponse_Struct)) break;
		const auto* brs = reinterpret_cast<const ::BeggingResponse_Struct*>(app->pBuffer);
#pragma pack(push, 1)
		struct VBegStruct {
			int32_t target;
			int32_t begger;
			int8_t  skill;
			int8_t  success;
			int16_t time;
			int32_t coins;
			int8_t  unknown[2];
		};
#pragma pack(pop)
		static_assert(sizeof(VBegStruct) == 18, "Trilogy Beg_Struct must be 18 bytes");
		VBegStruct beg{};
		beg.target  = GetTarget() ? static_cast<int32_t>(TranslateId(GetTarget()->GetID())) : 0;
		beg.begger  = static_cast<int32_t>(TranslateId(static_cast<uint32_t>(GetID())));
		beg.skill   = static_cast<int8_t>(GetSkill(EQ::skills::SkillBegging));
		beg.success = static_cast<int8_t>(brs->Result);
		beg.time    = 0;
		beg.coins   = static_cast<int32_t>(brs->Amount);
		m_tzs->SendToSession(m_session_key, 0x2521,
		                     reinterpret_cast<const uint8_t*>(&beg),
		                     static_cast<uint32_t>(sizeof(beg)));
		// Advance the money-display baseline by the amount we just told the
		// client to credit so the next Tick reconciliation doesn't re-push it.
		// Without this the begged coins land on the v29c coin counter twice —
		// once locally (the client adds `coins` of denomination `success` on
		// receipt) and once via Tick when it sees m_pp.copper / m_pp.silver
		// has incremented.  Success codes: 1=pp, 2=gp, 3=sp, 4=cp.
		if (brs->Result != 0 && brs->Amount != 0) {
			int32_t dcp = 0, dsp = 0, dgp = 0, dpp = 0;
			switch (brs->Result) {
				case 1: dpp = static_cast<int32_t>(brs->Amount); break; // platinum
				case 2: dgp = static_cast<int32_t>(brs->Amount); break; // gold
				case 3: dsp = static_cast<int32_t>(brs->Amount); break; // silver
				case 4: dcp = static_cast<int32_t>(brs->Amount); break; // copper
				default: break;
			}
			m_tzs->AdvanceMoneyBaseline(m_session_key, dcp, dsp, dgp, dpp);
		}
		break;
	}
	case OP_RequestClientZoneChange: {
		// Translate EQEmu's RequestClientZoneChange (Titanium) to Trilogy's OP_TeleportPC (0x4d21).
		// The Trilogy client automatically zones or intra-zone teleports based on whether the
		// zone name in TeleportPC matches the current zone.
		if (app->size < sizeof(RequestClientZoneChange_Struct)) break;
		const auto* rc = reinterpret_cast<const RequestClientZoneChange_Struct*>(app->pBuffer);
		const char* zone_name = ZoneName(static_cast<uint32>(rc->zone_id));
		if (!zone_name) break;
		Trilogy::structs::TeleportPC_Struct tpc{};
		memset(&tpc, 0, sizeof(tpc));
		strncpy(tpc.zone, zone_name, sizeof(tpc.zone) - 1);
		tpc.yPos    = rc->y;
		tpc.xPos    = rc->x;
		tpc.zPos    = (rc->z == 0.0f) ? 0.1f : rc->z;
		// Send EQEmu heading (0-512) directly; client divides by 2 to get 0-255 range.
		tpc.heading = rc->heading;
		m_tzs->SendToSession(m_session_key, 0x4d21,
		                     reinterpret_cast<const uint8_t*>(&tpc),
		                     static_cast<uint32_t>(sizeof(tpc)));
		break;
	}
	case OP_ZonePlayerToBind: {
		// Death / bind-point zoning: convert to Trilogy OP_TeleportPC (0x4d21).
		// ZonePlayerToBind_Struct has a variable-length zone_name tail; we only need
		// the fixed header (bind_zone_id, coords, heading).
		if (app->size < sizeof(::ZonePlayerToBind_Struct)) break;
		const auto* zpb = reinterpret_cast<const ::ZonePlayerToBind_Struct*>(app->pBuffer);
		const bool same_zone = (zpb->bind_zone_id == 0 ||
		                        static_cast<uint32>(zpb->bind_zone_id) == GetZoneID());
		if (same_zone) {
			// Same-zone death respawn.  ZonePC has already overwritten m_Position
			// with bind coords, so zpb->x/y/z and GetX/GetY/GetZ are both bind
			// coords.  We must still send TeleportPC (the client needs it to enter
			// zone-change-pending state and respond with 0xa320), but use the
			// death position saved by HandleBecomeCorpse — this is where the
			// player already is visually, so no visible teleport occurs.
			// The client then responds with 0xa320 → HandleZoneChange drives the
			// normal zone-out flow (DoZoneSuccess → world → 0x0480 → A320+CLOSE
			// → reconnect at bind).
			const char* zname = ZoneName(static_cast<uint32>(GetZoneID()));
			if (!zname) break;
			Trilogy::structs::TeleportPC_Struct tpc{};
			memset(&tpc, 0, sizeof(tpc));
			strncpy(tpc.zone, zname, sizeof(tpc.zone) - 1);
			if (m_has_death_pos) {
				tpc.yPos    = m_death_y;
				tpc.xPos    = m_death_x;
				tpc.zPos    = (m_death_z == 0.0f) ? 0.1f : m_death_z;
				tpc.heading = m_death_heading;
				m_has_death_pos = false;
			} else {
				tpc.yPos    = zpb->y;
				tpc.xPos    = zpb->x;
				tpc.zPos    = (zpb->z == 0.0f) ? 0.1f : zpb->z;
				tpc.heading = zpb->heading;
			}
			m_tzs->SendToSession(m_session_key, 0x4d21,
			                     reinterpret_cast<const uint8_t*>(&tpc),
			                     static_cast<uint32_t>(sizeof(tpc)));
			break;
		}
		// Cross-zone death: convert to TeleportPC (0x4d21).  The different zone
		// name triggers the client to send 0xa320 → HandleZoneChange → full zone-out.
		const char* zname = ZoneName(static_cast<uint32>(zpb->bind_zone_id));
		if (!zname) break;
		Trilogy::structs::TeleportPC_Struct tpc{};
		memset(&tpc, 0, sizeof(tpc));
		strncpy(tpc.zone, zname, sizeof(tpc.zone) - 1);
		tpc.yPos    = zpb->y;
		tpc.xPos    = zpb->x;
		tpc.zPos    = (zpb->z == 0.0f) ? 0.1f : zpb->z;
		tpc.heading = zpb->heading;
		m_tzs->SendToSession(m_session_key, 0x4d21,
		                     reinterpret_cast<const uint8_t*>(&tpc),
		                     static_cast<uint32_t>(sizeof(tpc)));
		break;
	}
	case OP_ZoneChange: {
		// Server→client zone-change approval: translate EQEmu's 88-byte Titanium
		// ZoneChange_Struct to the 68-byte Trilogy ZoneChange_Struct (opcode 0xa320).
		// On receipt the Trilogy client disconnects from the current zone server and
		// connects to the zone whose IP:port it received in 0x0480 from the world server.
		//
		// CRASH PREVENTION (0x004c7752 / 0xff000082):
		// When 0xa320 arrives with zone_name="freportw", the Trilogy client looks up
		// "freportw" in its EQNetwork connection table.  On a revisited zone, the entry
		// holds a freed-pointer sentinel (0xff000000) left over from the previous visit.
		// Reading entry+0x82 (the connection-object pointer) with this value crashes.
		//
		// Fix: send a server-initiated EQNetwork CLOSE immediately after 0xa320.
		// CLOSE is ARQ-sequenced after 0xa320, so the client processes 0xa320 first
		// (starts connecting to the new zone) and then CLOSE (cleanly nulls out this
		// zone's connection-table entry, entry.connection = NULL).  On the next visit
		// 0xa320 finds NULL → creates a fresh connection → no crash.
		//
		// Timed broadcasts (stamina, time-of-day) are also suppressed in Tick() while
		// m_is_zoning=true; packets arriving during the close handshake can corrupt
		// EQNetwork's cleanup and cause it to set the sentinel instead of NULL.
		if (app->size < sizeof(::ZoneChange_Struct)) break;
		const auto* emu = reinterpret_cast<const ::ZoneChange_Struct*>(app->pBuffer);
		if (emu->success < 0) break; // denied — Trilogy has no error display for this
		const char* dest_zone = ZoneName(static_cast<uint32>(emu->zoneID));
		if (!dest_zone) break;
		Trilogy::structs::ZoneChange_Struct trio_zc{};
		memset(&trio_zc, 0, sizeof(trio_zc));
		strncpy(trio_zc.char_name, emu->char_name, sizeof(trio_zc.char_name) - 1);
		strncpy(trio_zc.zone_name,  dest_zone,      sizeof(trio_zc.zone_name)  - 1);
		// Magic bytes observed in EQClassic ProcessOP_ZoneChange success responses.
		static const uint8_t kMagic[20] = {
			0x10, 0x00, 0x00, 0x00, 0x04, 0xb5, 0x01, 0x02, 0x43, 0x58,
			0x4f, 0x00, 0xb0, 0xa5, 0xc7, 0x0d, 0x01, 0x00, 0x00, 0x00
		};
		memcpy(trio_zc.zc_unknown1, kMagic, sizeof(kMagic));
		// EQClassic sends an empty 0x1020 packet immediately before the A320 approval.
		m_tzs->SendToSession(m_session_key, 0x1020, nullptr, 0);
		m_tzs->SendToSession(m_session_key, 0xa320,
		                     reinterpret_cast<const uint8_t*>(&trio_zc),
		                     static_cast<uint32_t>(sizeof(trio_zc)));
		// ARQ-sequenced CLOSE: arrives at client after 0xa320, causes EQNetwork to
		// null out this zone's connection-table entry cleanly (see comment above).
		m_tzs->SendCloseToSession(m_session_key);
		// Enter zoning state immediately: the client is tearing down its connection.
		// Suppress further broadcast traffic (mob heartbeats, NPC spawns, ground items)
		// so they don't corrupt the client's teardown state machine.
		m_is_zoning = true;
		m_deferred_spawns.clear(); // discard; this session won't deliver them
		m_deferred_player_spawns.clear();
		break;
	}
	default:
		// Opcodes without a Trilogy translation are silently dropped.
		break;
	}
}

// ============================================================
// HandleNewSpawn — translate OP_NewSpawn (internal format) to
// Trilogy wire format and send to this client via EQNetwork.
//
// The incoming packet is in EQEmu's internal NewSpawn_Struct.
// We extract spawnId, look up the Mob, build a Trilogy
// Spawn_Struct, apply CRC32 and cipher, then call SendToSession.
// ============================================================

void TrilogyClient::HandleNewSpawn(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::NewSpawn_Struct)) return;

	const auto* ns = reinterpret_cast<const ::NewSpawn_Struct*>(app->pBuffer);
	uint16 spawn_id = static_cast<uint16>(ns->spawn.spawnId);

	// Don't echo our own spawn back to ourselves.
	if (spawn_id == GetID()) return;

	// Look up the mob so we can read its current state.
	Mob* mob = entity_list.GetMob(spawn_id);
	if (!mob) return;

	// Diag: FV Maiden's Voyage invisibility. Log every mid-session 4921 for
	// NPC 84250 with server-side state. See Zone:TrilogyBoatDiag.
	if (RuleB(Zone, TrilogyBoatDiag) && mob->IsNPC() &&
	    mob->CastToNPC()->GetNPCTypeID() == 84250) {
		LogInfo("[BoatDiag] HandleNewSpawn 4921 Maiden 84250: sid={} pos=({:.1f},{:.1f},{:.1f}) "
		        "h={:.1f} moving={} grid={} m_is_zoning={} char=[{}]",
		        spawn_id, mob->GetX(), mob->GetY(), mob->GetZ(),
		        mob->GetHeading(),
		        mob->IsMoving() ? 1 : 0,
		        mob->CastToNPC()->GetGrid(),
		        m_is_zoning ? 1 : 0,
		        GetCleanName());
	}

	// Players, Bots, and Playerbots are sent via 0x6121 (ZN_OP_ZoneSpawns) so
	// the Trilogy client treats them as zone-permanent and never stales them
	// out.  Regular NPCs use 0x4921 (ZN_OP_NewSpawn) which is fine since they
	// appear in the A120 heartbeat whenever they move.
	//
	// Bots (the Bot subsystem — distinct from Playerbots) must reach the
	// client as NPC=0 (player nameplate) so Trilogy players can target them
	// for /invite and the BotAI takes over as a real group member; otherwise
	// v29c renders them with the red NPC nameplate and refuses group invites.
	bool is_playerbot = mob->IsNPC() &&
	                    mob->CastToNPC()->GetNPCTypeID() == static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId));
	bool is_bot       = mob->IsBot();

	if (mob->IsClient() || is_playerbot || is_bot) {
		// Player/playerbot/bot spawns require multi-packet sequences
		// (ZoneSpawns bulk + illusion + WearChange) that cannot be trivially
		// buffered as a single wire packet.  During zone transition, defer
		// the dispatch by entity ID; OnClientReady will re-resolve the Mob*
		// and call the appropriate permanent-send once m_is_zoning clears.
		// Without the deferred path, Bots loaded by Bot::LoadAndSpawnAllZonedBots
		// during the owner's zone-in restoration block evaporate here and the
		// owner sees no party at the destination.
		if (m_is_zoning) {
			if (m_deferred_player_spawns.size() < kMaxDeferredSpawns) {
				m_deferred_player_spawns.push_back(spawn_id);
			}
			return;
		}
		if (mob->IsClient())
			m_tzs->SendPlayerSpawnPermanent(m_session_key, mob->CastToClient());
		else
			m_tzs->SendPlayerbotSpawnPermanent(m_session_key, mob->CastToNPC());
		return;
	}

	// Corpses spawned via Corpse::Spawn() (DB load, cross-zone move, /corpse summon).
	// Build a Trilogy corpse spawn with NPC=2 (NPC corpse) or NPC=3 (player corpse)
	// and send via the permanent ZoneSpawns opcode so the client doesn't stale them.
	if (mob->IsCorpse()) {
		Corpse* corpse = mob->CastToCorpse();
		m_tzs->SendCorpseSpawnPermanent(m_session_key, corpse);
		return;
	}

	Trilogy::structs::NewSpawn_Struct out{};
	memset(&out, 0, sizeof(out));
	// out.ns_unknown1 will be filled by CRC32::SetEQChecksum below.

	Trilogy::structs::Spawn_Struct& sp = out.spawn;

	sp.size      = mob->GetSize();
	if (sp.size <= 0.0f) sp.size = 6.0f;
	// Speed encoding: derive from DB to match the bulk OP_ZoneSpawns path
	// (trilogy_zone.cpp:6953-6954).  The previous hardcoded 0.7/1.4 fit
	// typical humanoids but broke any NPC with atypical speeds — most
	// visibly boats (race 72, runspeed=12.5 in DB) respawned mid-session
	// via quest::spawn2.  Once the client cached runspeed=1.4 for such a
	// mob, subsequent A120 position broadcasts moving it at ~29 u/s far
	// exceeded that speed model and v29c stopped rendering the entity.
	// Gate behind Zone:TrilogyNewSpawnSpeedFromDB so a regression can be
	// reverted at runtime via `#reloadrules` without a rebuild.
	// ToTrilogySpeed = eqemu_speed / 40.0f (mirrors trilogy_zone.cpp:298).
	if (RuleB(Zone, TrilogyNewSpawnSpeedFromDB)) {
		sp.walkspeed = (static_cast<float>(mob->GetBaseWalkspeed()) / 40.0f) * 1.5f;
		sp.runspeed  =  static_cast<float>(mob->GetBaseRunspeed())  / 40.0f;
	} else {
		sp.walkspeed = 0.7f;
		sp.runspeed  = 1.4f;
	}
	sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(mob->GetHeading() / 2.0f));
	sp.y_pos     = static_cast<int16_t>(mob->GetY());
	sp.x_pos     = static_cast<int16_t>(mob->GetX());
	sp.z_pos     = static_cast<int16_t>(mob->GetZ() * 10.0f);
	sp.spawn_id  = static_cast<int16_t>(spawn_id);
	sp.body_type = static_cast<int16_t>(mob->GetBodyType());
	sp.cur_hp    = static_cast<int16_t>(mob->GetHPRatio());
	sp.GuildID   = static_cast<uint16_t>(0xFFFF); // no guild by default
	sp.race      = static_cast<int8_t>(mob->GetRace());
	sp.NPC       = 1; // regular NPC (players and Playerbots returned early above)
	// Translate EQEmu class id → Trilogy (Merchant 41→32 so the client opens the
	// shop on right-click, Banker 40→16, GM trainers 20-34→17-31).
	sp.class_    = static_cast<int8_t>(Trilogy::structs::TranslateClassToTrilogy(mob->GetClass()));
	sp.gender    = static_cast<int8_t>(mob->GetGender());
	sp.level     = static_cast<int8_t>(mob->GetLevel());
	sp.anim_type = 0x64; // standing (EQClassic hardcodes 100)
	sp.light     = static_cast<int8_t>(mob->GetEquipmentLightType());
	sp.guildrank = static_cast<int8_t>(0xFF);

	if (IsPlayerRace(mob->GetRace())) {
		sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
		// Helm: v29c does NOT render the helm via equipment[0] for NPC=1 entities
		// even though it renders the body via the equipment[1..6] path.  Drive the
		// helm explicitly via npc_helm_graphic when helmtexture is set (1..7); fall
		// back to 0xFF so a loot-helm item's material in equipment[0] can still
		// drive the helm for NPCs whose helmtexture is left at 0 in npc_types.
		const uint8_t tex     = mob->GetTexture();
		const uint8_t helmtex = mob->GetHelmTexture();
		sp.npc_helm_graphic  = (helmtex == 0 || helmtex > 7)
		                           ? static_cast<int8_t>(0xFF)
		                           : static_cast<int8_t>(helmtex);
		// Texture fallback: when GetEquipmentMaterial(slot) returns 0 (no loot
		// equipped in that slot), substitute the NPC's body texture so the
		// whole body shows uniform armor coverage instead of partial nakedness.
		// Mirrors the bulk SendZoneSpawns path at trilogy_zone.cpp:6347-6352.
		// Playerbots return early via SendPlayerbotSpawnPermanent above, so
		// any mob reaching this branch is a regular NPC — no is_playerbot
		// check needed.
		for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
			uint8_t mat = mob->GetEquipmentMaterial(static_cast<uint8_t>(mi));
			if (mat == 0) {
				const uint8_t fb = (mi == 0) ? helmtex : tex; // slot 0 = helm
				if (fb > 0 && fb < 0xFF) mat = fb;
			}
			sp.equipment[mi]   = static_cast<int8_t>(mat);
			sp.equipcolors[mi] = static_cast<int32_t>(
				Trilogy::NormalizeTintColor(mob->GetEquipmentColor(static_cast<uint8_t>(mi))));
		}
	} else {
		uint8_t tex     = mob->GetTexture();
		uint8_t helmtex = mob->GetHelmTexture();
		sp.npc_armor_graphic = (tex > 7) ? static_cast<int8_t>(0xFF) : static_cast<int8_t>(tex);
		sp.npc_helm_graphic  = (helmtex > 7) ? static_cast<int8_t>(0xFF) : static_cast<int8_t>(helmtex);
	}
	sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(mob->GetEquipmentMaterial(EQ::textures::weaponPrimary));
	sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(mob->GetEquipmentMaterial(EQ::textures::weaponSecondary));
	// equipment[7]=primary, equipment[8]=secondary.  v29c has no Range visual slot
	// — bows in slotRange are NOT rendered on the player.  EQClassic's own
	// MakeSpawnUpdate (Zone/Source/client.cpp:1832-1846) reads pp.inventory[13]/[14]
	// directly with no range fallback; matching that behaviour keeps the primary
	// hand empty when the only weapon is a bow.  NPC ranger trainers render bows
	// because their loadout puts the bow into equipment[7], not because the client
	// substitutes from range.

	if (mob->IsPlayerCorpse()) {
		// GetCleanName() strips apostrophe+space from "Name's corpse" → "Namescorpse".
		// Use backtick+underscore format that CleanMobName preserves: "Name`s_corpse".
		char raw_copy[64]{};
		strncpy(raw_copy, mob->GetName(), sizeof(raw_copy) - 1);
		EntityList::RemoveNumbers(raw_copy);
		char* apos = strchr(raw_copy, '\'');
		if (apos) {
			*apos = '\0';
			char cname[64]{};
			snprintf(cname, sizeof(cname), "%s`s_corpse", raw_copy);
			strncpy(sp.name, cname, sizeof(sp.name) - 1);
		} else {
			strncpy(sp.name, mob->GetCleanName(), sizeof(sp.name) - 1);
		}
	} else {
		strncpy(sp.name, TrilogyWireName(mob), sizeof(sp.name) - 1);
	}
	strncpy(sp.Surname, mob->GetLastName(),  sizeof(sp.Surname) - 1);

	// CRC32 over bytes [4..168) stored in out.ns_unknown1 (bytes [0..3]).
	CRC32::SetEQChecksum(reinterpret_cast<unsigned char*>(&out), sizeof(out));

	// Encrypt in-place; 168 bytes is already a multiple of 4.
	EncryptSpawnPacket(reinterpret_cast<uint8_t*>(&out), static_cast<uint32_t>(sizeof(out)));

	if (m_is_zoning) {
		if (m_deferred_spawns.size() < kMaxDeferredSpawns) {
			m_deferred_spawns.emplace_back(uint16_t{0x4921},
				std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&out),
				                     reinterpret_cast<const uint8_t*>(&out) + sizeof(out)));
			// Record now — the deferred drain in OnClientReady sends via
			// SendToSession with an opaque encrypted payload, so the ghost
			// reconcile path won't see the spawn_id at replay time.
			m_tzs->NoteKnownSpawn(m_session_key, static_cast<uint16_t>(spawn_id));
		}
		return;
	}

	// Diag: log NPC properties when broadcasting NewSpawn — correlates a fresh
	// mob arrival with the freeze trigger so we can identify what's unusual
	// about a specific named when it enters the player's view.
	LogInfo("[TrilogyDiag] 4921 NewSpawn sid={} name='{}' race={} class={} level={} gender={} texture={} helmtex={} size={} npctypeid={} player_race={}",
	        static_cast<int>(sp.spawn_id),
	        mob->GetCleanName(),
	        static_cast<int>(mob->GetRace()),
	        static_cast<int>(mob->GetClass()),
	        static_cast<int>(mob->GetLevel()),
	        static_cast<int>(mob->GetGender()),
	        static_cast<int>(mob->GetTexture()),
	        static_cast<int>(mob->GetHelmTexture()),
	        static_cast<double>(mob->GetSize()),
	        mob->IsNPC() ? static_cast<int>(mob->CastToNPC()->GetNPCTypeID()) : -1,
	        IsPlayerRace(mob->GetRace()) ? 1 : 0);

	m_tzs->SendToSession(m_session_key, 0x4921,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
	m_tzs->NoteKnownSpawn(m_session_key, static_cast<uint16_t>(spawn_id));

	// Seed v29c-client-known-material model from the spawn struct equipment.
	SeedKnownMaterials(static_cast<uint16_t>(spawn_id), sp.equipment);

	// Explicit OP_WearChange for the helm slot — v29c does not render the helm
	// from the spawn struct's npc_helm_graphic field for player-race NPC=1
	// entities while the body is in player-equipment mode (npc_armor_graphic=
	// 0xFF + equipment[1..6]).  A follow-up WearChange for wear_slot=0 (head)
	// with the helmtexture as the slot graphic forces the helm to render
	// without disrupting the body mechanism.  Players and Playerbots returned
	// early before reaching this point (SendPlayerSpawnPermanent /
	// SendPlayerbotSpawnPermanent handle them), so any IsPlayerRace mob here
	// is a regular NPC.
	if (IsPlayerRace(mob->GetRace())) {
		// Follow-up OP_Illusion (0x9120) so v29c picks up face/race for this
		// player-race NPC.  The NewSpawn packet doesn't carry face data;
		// bulk zone-in NPCs get their face via the post-D820 illusion loop,
		// mid-session respawns need this follow-up.  Target uses
		// TrilogyWireName so duplicate-named NPCs (Dervish Cutthroats etc.)
		// route the face update to the right instance via v29c's by-name
		// lookup of the raw MakeNameUnique-suffixed wire name.
		// texture/helm use 0xFFFF (-1) keep-current sentinel so the body
		// rendering established by the spawn struct's equipment[1..6] and
		// the helm WearChange below are not disturbed.
		uint8_t il_buf[72];
		memset(il_buf, 0, sizeof(il_buf));
		const char* wire_name = TrilogyWireName(mob);
		const size_t nlen = strlen(wire_name);
		memcpy(il_buf,      wire_name, nlen < 29 ? nlen : 29); // name @ 0
		memcpy(il_buf + 30, wire_name, nlen < 15 ? nlen : 15); // target @ 30
		il_buf[48] = 24; il_buf[49] = 0;                       // jackbauer = 24 LE
		auto put_le16 = [&](size_t off, int16_t v) {
			uint16_t u = static_cast<uint16_t>(v);
			il_buf[off]     = static_cast<uint8_t>(u);
			il_buf[off + 1] = static_cast<uint8_t>(u >> 8);
		};
		put_le16(62, static_cast<int16_t>(mob->GetRace()));
		put_le16(64, static_cast<int16_t>(mob->GetGender()));
		put_le16(66, static_cast<int16_t>(-1));                 // keep current texture
		put_le16(68, static_cast<int16_t>(-1));                 // keep current helm
		put_le16(70, static_cast<int16_t>(mob->GetLuclinFace()));
		m_tzs->SendToSession(m_session_key, 0x9120, il_buf, 72);

		const uint8_t helmtex = mob->GetHelmTexture();
		if (helmtex > 0 && helmtex < 0xFF) {
			// Update the model: the helm now reflects helmtex, not equipment[0].
			RecordKnownMaterial(static_cast<uint16_t>(spawn_id), 0, helmtex);
			Trilogy::structs::WearChange_Struct wc{};
			wc.spawn_id     = static_cast<int32_t>(spawn_id);
			wc.wear_slot_id = 0; // head slot
			wc.slot_graphic = static_cast<int8_t>(helmtex);
			wc.sub_op       = 0;
			wc.color        = static_cast<int32_t>(
				Trilogy::NormalizeTintColor(mob->GetEquipmentColor(EQ::textures::armorHead)));
			m_tzs->SendToSession(m_session_key, 0x9220,
			                     reinterpret_cast<const uint8_t*>(&wc),
			                     static_cast<uint32_t>(sizeof(wc)));
		}
	}
}

// ============================================================
// HandleDeleteSpawn — translate OP_DeleteSpawn (internal format)
// to Trilogy wire format.
// ============================================================

void TrilogyClient::HandleDeleteSpawn(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::DeleteSpawn_Struct)) return;

	const auto* ds_in = reinterpret_cast<const ::DeleteSpawn_Struct*>(app->pBuffer);
	uint16 spawn_id = static_cast<uint16>(ds_in->spawn_id);

	// Don't send delete for our own entity.
	if (spawn_id == GetID()) return;

	Trilogy::structs::DeleteSpawn_Struct ds_out{};
	memset(&ds_out, 0, sizeof(ds_out));
	ds_out.spawn_id = static_cast<int16_t>(spawn_id);
	// ds_out.ds_unknown1 = 0

	m_tzs->SendToSession(m_session_key, 0x2b20,
	                     reinterpret_cast<const uint8_t*>(&ds_out),
	                     static_cast<uint32_t>(sizeof(ds_out)));
	m_tzs->ForgetKnownSpawn(m_session_key, static_cast<uint16_t>(spawn_id));

	// Drop any cached SpawnAppearance state for this spawn so a future
	// reuse of the id (rare but possible) starts fresh.
	m_last_appearance.erase(static_cast<uint16_t>(spawn_id));
	m_mob_update_last.erase(static_cast<uint16_t>(spawn_id));
	m_movement_anim_cache.erase(static_cast<uint16_t>(spawn_id));

	// Drop all per-slot known-material entries for this spawn — the next
	// reuse of spawn_id will need a fresh seed from its spawn struct's
	// equipment[9].  Key encoding is (spawn_id << 8) | slot, so we walk
	// and prune (range erase isn't natural on unordered_map).
	const uint32_t lo = static_cast<uint32_t>(spawn_id) << 8;
	const uint32_t hi = lo | 0xFFu;
	for (auto it = m_client_known_material.begin(); it != m_client_known_material.end(); ) {
		if (it->first >= lo && it->first <= hi) it = m_client_known_material.erase(it);
		else ++it;
	}
}

// ============================================================
// HandleIllusion — translate OP_Illusion (EQEmu internal) to the
// 72-byte EQClassic Zone wire format (opcode 0x9120) so the Trilogy
// client applies live appearance changes (face, texture, etc.)
// without requiring a full respawn.
//
// The Trilogy client identifies the target entity by NAME, not spawn_id.
// jackbauer (offset 48) must be 24 or the client ignores the packet.
// ============================================================

void TrilogyClient::HandleIllusion(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Illusion_Struct)) return;

	const auto* emu = reinterpret_cast<const ::Illusion_Struct*>(app->pBuffer);

	// EQEmu's Mob::SendIllusionPacket sets emu->charname to the already-cleaned
	// name (GetCleanName).  For v29c's by-name illusion lookup to land on the
	// correct entity when duplicates exist, we need the raw MakeNameUnique-
	// suffixed name — same string the spawn struct sent.  Resolve via spawnid.
	Mob* tgt = entity_list.GetMob(static_cast<uint16>(emu->spawnid));
	const char* wire_name = tgt ? TrilogyWireName(tgt) : emu->charname;

	// Build a raw 72-byte buffer so the name can exceed the 15-char struct field
	// limit.  EQClassic's SendIllusionPacket uses strcpy (no length limit), which
	// means the Trilogy client reads the name as null-terminated from offset 0.
	// Names up to 29 chars safely fit before the target field at offset 30.
	uint8_t out[72];
	memset(out, 0, 72);
	size_t nlen = strlen(wire_name);
	memcpy(out,      wire_name, nlen < 29 ? nlen : 29); // name at offset 0
	memcpy(out + 30, wire_name, nlen < 15 ? nlen : 15); // target at offset 30
	out[48] = 24; out[49] = 0;                               // jackbauer = 24 LE
	int16_t race = static_cast<int16_t>(emu->race);
	int16_t gender = static_cast<int16_t>(emu->gender);
	int16_t texture = static_cast<int16_t>(emu->texture);
	int16_t helm = static_cast<int16_t>(emu->helmtexture);
	int16_t face = static_cast<int16_t>(emu->face);
	out[62] = static_cast<uint8_t>(static_cast<uint16_t>(race));    out[63] = static_cast<uint8_t>(static_cast<uint16_t>(race)    >> 8);
	out[64] = static_cast<uint8_t>(static_cast<uint16_t>(gender));  out[65] = static_cast<uint8_t>(static_cast<uint16_t>(gender)  >> 8);
	out[66] = static_cast<uint8_t>(static_cast<uint16_t>(texture)); out[67] = static_cast<uint8_t>(static_cast<uint16_t>(texture) >> 8);
	out[68] = static_cast<uint8_t>(static_cast<uint16_t>(helm));    out[69] = static_cast<uint8_t>(static_cast<uint16_t>(helm)    >> 8);
	out[70] = static_cast<uint8_t>(static_cast<uint16_t>(face));    out[71] = static_cast<uint8_t>(static_cast<uint16_t>(face)    >> 8);

	m_tzs->SendToSession(m_session_key, 0x9120, out, 72);
}

// ============================================================
// HandleClientUpdate — OP_ClientUpdate per-mob position broadcast.
//
// EQClassic-faithful event-driven A120 path. EQEmu's MovementManager
// fires OP_ClientUpdate frequently per moving mob (observed 100-500/sec
// aggregate in ecommons during prior testing). We throttle PER MOB to
// ~4 Hz via m_mob_update_last; combined with the 600u cull from this
// client's position, total A120 emission stays bounded by the number of
// in-view moving mobs, not by total zone NPC count.
//
// 2026-06-23: accepted updates are pushed to m_pending_mob_updates and
// drained by FlushPendingMobUpdates() once per Tick. The 25-entries-per-
// A120 batching cuts moving-mob ARQ count by ~20×, which is the right
// architectural lever against the v29c ~4870-ARQ session ceiling
// (see [[project-trilogy-resend-explosion]] lesson #2).
// ============================================================

void TrilogyClient::HandleClientUpdate(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(PlayerPositionUpdateServer_Struct)) return;
	const auto* p = reinterpret_cast<const PlayerPositionUpdateServer_Struct*>(app->pBuffer);

	const uint16_t spawn_id = p->spawn_id;
	if (spawn_id == 0) return;

	Mob* m = entity_list.GetMob(spawn_id);
	if (!m) return;

	// Self-echo: normally the v29c client tracks its own position locally
	// from F320 input; a server echo would rubber-band.  BUT when the server
	// takes movement authority — SE_Fear locks controls via AI_Start's
	// SpawnAppearance(Animation=14, Freeze=102), then MovementManager drives
	// position via SetPosition every AI_movement_timer tick — the client has
	// no local input to advance its position, so dropping the echo leaves it
	// frozen in place (user-visible "feared but standing still, like stunned").
	//
	// EQClassic parity: SpellEffects.cpp SE_Fear branch flips SetFeared(true)
	// + SendAppearance(SAT_Position_Update=14, SAPP_Lose_Control=102) — same
	// wire values as AppearanceType::Animation + Animation::Freeze — then
	// FearMovement() ticks server-side and sends OP_ClientUpdate (0xf320) with
	// the new position, which the v29c client accepts as an authoritative XY
	// correction while feared (see [[project-trilogy-skyshrine-pads]] for the
	// "0xf320 is XY-only" gotcha — perfect fit for fear which runs on terrain).
	if (m == static_cast<Mob*>(this)) {
		if (IsFeared()) {
			SendForcedSelfPositionUpdate(p);
		}
		return;
	}

	// Cull from this client's current position.  600u matches the old
	// SendMobHeartbeat cull and EQClassic's effective broadcast radius
	// for QueueCloseClients (default 600u).
	static constexpr float kCullSq = 600.0f * 600.0f;
	const float dx = m->GetX() - GetX();
	const float dy = m->GetY() - GetY();
	const float dist_sq_client_update = dx * dx + dy * dy;
	// Diag: FV Maiden. Log EVERY outgoing OP_ClientUpdate (A120 candidate)
	// including cull outcome so we can see whether the boat's position
	// broadcasts are being culled by distance. See Zone:TrilogyBoatDiag.
	if (RuleB(Zone, TrilogyBoatDiag) && m->IsNPC() &&
	    m->CastToNPC()->GetNPCTypeID() == 84250) {
		LogInfo("[BoatDiag] HandleClientUpdate outgoing Maiden 84250: sid={} "
		        "pos=({:.1f},{:.1f},{:.1f}) player=({:.1f},{:.1f}) dist={:.1f} "
		        "moving={} anim_in={} culled={} char=[{}]",
		        spawn_id, m->GetX(), m->GetY(), m->GetZ(),
		        GetX(), GetY(),
		        std::sqrt(dist_sq_client_update),
		        m->IsMoving() ? 1 : 0,
		        static_cast<int>(p->animation),
		        (dist_sq_client_update > kCullSq) ? 1 : 0,
		        GetCleanName());
	}
	if (dist_sq_client_update > kCullSq) return;

	// Per-mob throttle.  EXPERIMENT 2026-06-27: dropped 2000 → 250 ms now
	// that SendMobHeartbeat skips moving NPCs entirely.  The previous
	// wobble at sub-second rates came from heartbeat firing delta=0
	// between our event-driven delta-bearing broadcasts — the alternation
	// collapsed client extrapolation back to anim_type-only every other
	// update.  With heartbeat out of the way, the event-driven path is
	// the sole source for moving NPCs, and 4 Hz updates with consistent
	// real deltas should let the client interpolate cleanly.
	//
	// Additionally: anim_type-change bypass below skips this throttle on
	// state transitions (walk→run on aggro, run→walk on disengage,
	// moving→stopped at waypoint) so the client never extrapolates with
	// a stale speed for up to a full throttle interval — that was the
	// root cause of the visible "snap forward on aggro" symptom.
	static constexpr uint64_t kMinIntervalMs = 250;
	const uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	// Compute the wire anim_type up front so we can compare against the
	// last-cached value and bypass the throttle on transitions.
	Trilogy::structs::SpawnPositionUpdate_Struct upd{};
	upd.spawn_id = static_cast<int16_t>(TranslateId(static_cast<uint32_t>(spawn_id)));
	upd.heading  = static_cast<int8_t>(static_cast<uint8_t>(m->GetHeading() / 2.0f));
	upd.y_pos    = static_cast<int16_t>(m->GetY());
	upd.x_pos    = static_cast<int16_t>(m->GetX());
	upd.z_pos    = static_cast<int16_t>(m->GetZ() * 10.0f);

	if (m->IsMoving() || p->animation != 0) {
		upd.anim_type = TrilogyZoneServer::EncodeTrilogyAnim(
			m, static_cast<int>(p->animation));
	}

	// Throttle gate — but bypass if anim_type differs from the last cached
	// value for this spawn (state transition).  Transitions need immediate
	// delivery; the throttle is only there to cap steady-state rate.
	auto cache_it = m_movement_anim_cache.find(static_cast<uint16_t>(spawn_id));
	const bool anim_changed =
		(cache_it == m_movement_anim_cache.end()) ||
		(cache_it->second.anim != upd.anim_type);
	if (!anim_changed) {
		auto it = m_mob_update_last.find(spawn_id);
		if (it != m_mob_update_last.end() && now_ms - it->second < kMinIntervalMs)
			return;
	}
	m_mob_update_last[spawn_id] = now_ms;

	// Stash the MovementManager-authoritative anim so the heartbeat path
	// (SendMobHeartbeat in trilogy_zone.cpp) doesn't override walking
	// patrol NPCs with "run" or running Playerbots with "walk" via its
	// IsEngaged()-based heuristic.  Cache age-bounded inside the getter.
	m_movement_anim_cache[static_cast<uint16_t>(spawn_id)] =
		MovementAnim{ upd.anim_type, now_ms };

	// Per-tick velocity vector from EQEmu's MobMovementManager (filled by
	// FillCommandStruct → FloatToEQ13(delta) = delta * 64).  Titanium
	// client consumes these same delta_x/y/z values to interpolate
	// position between OP_ClientUpdate broadcasts — that's why Titanium
	// NPCs look smooth on the same server that produces jaggy NPCs on
	// v29c.  Pass through directly into the v29c 10-bit signed bitfield
	// (PackDelta10 clamps to [-512, 511]; typical run velocities encode
	// to ~45, well within range; only fall/knockback would clamp).
	//
	// Why this isn't the prior "strafing" failure:  earlier attempts in
	// SendMobHeartbeat computed a fake delta from heading × speed via
	// kVelocityWireScale, which strafed because the rendered heading
	// direction diverged from the actual server motion vector on any
	// tick with z-correction or sub-degree pathing drift.  EQEmu's
	// MovementManager-fed delta IS the actual motion vector the server's
	// pathing code is using, so direction matches by construction — the
	// v29c client's extrapolation lands on the server's next-tick
	// position rather than drifting off-axis.  This is the event-driven
	// path; SendMobHeartbeat's polled fallback keeps delta=0 for
	// stationary mobs (which is correct — no extrapolation hint needed
	// when there's no motion).
	// EXPERIMENT 2026-06-27: 2× delta magnitude to test whether v29c
	// expects a different fixed-point scale than Titanium's FloatToEQ13
	// (×64).  If v29c internally divides incoming delta by, say, 128
	// instead of 64, our raw FloatToEQ13 values would render as half
	// the actual velocity — client extrapolates at half server speed
	// and we observe a forward snap every broadcast.  Doubling here
	// tests that hypothesis.  PackDelta10 clamps to ±511, so typical
	// walk/run values (45/90 baseline → 90/180 doubled) stay in range;
	// only severe knockback/fall would clamp.
	TrilogyZoneServer::EncodeTrilogyDelta(&upd,
	                                      static_cast<int32_t>(p->delta_x) * 2,
	                                      static_cast<int32_t>(p->delta_y) * 2,
	                                      static_cast<int32_t>(p->delta_z) * 2);

	// Dedup within the current Tick's flush window: if this spawn_id is
	// already queued, replace its entry in place so only the freshest state
	// hits the wire.  The 250ms per-mob throttle above caps *steady-state*
	// rate, but the anim_changed bypass right below the throttle lets state
	// transitions through unthrottled — under a 70-bot follow on the move,
	// bots frequently flip walk↔run to match owner speed, so a single Tick
	// can accumulate 2-3 updates per spawn.  Dedup collapses those to one
	// with the newest position/anim/delta; queue size is bounded by the
	// count of *distinct* visible-moving mobs rather than event volume.
	//
	// Cost: linear scan of the pending vector (typical size < 100 during
	// dense raid scenes), well inside the per-Tick budget.
	for (auto& existing : m_pending_mob_updates) {
		if (existing.spawn_id == upd.spawn_id) {
			existing = upd;
			return;
		}
	}
	m_pending_mob_updates.push_back(upd);
}

// ============================================================
// FlushPendingMobUpdates — drain m_pending_mob_updates into bulk A120
// packets (one per 25 entries). Called once per Tick by
// TrilogyZoneServer::Tick. Mirrors the wire format used by
// SendMobHeartbeat: [int32 count][SpawnPositionUpdate_Struct × count].
// ============================================================
void TrilogyClient::FlushPendingMobUpdates()
{
	if (m_pending_mob_updates.empty()) return;

	// Same cap as SendMobHeartbeat: keeps the datagram under 512 B
	// (4 + 25*15 = 379) so v29c never has to reassemble a fragmented A120.
	static constexpr size_t kMaxPerPkt = 25;

	const size_t total = m_pending_mob_updates.size();
	for (size_t off = 0; off < total; off += kMaxPerPkt) {
		const size_t   batch = std::min(kMaxPerPkt, total - off);
		const uint32_t plen  = static_cast<uint32_t>(
			4 + batch * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct));

		uint8_t pkt[4 + kMaxPerPkt * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)];
		int32_t n = static_cast<int32_t>(batch);
		memcpy(pkt, &n, 4);
		memcpy(pkt + 4,
		       m_pending_mob_updates.data() + off,
		       batch * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct));

		// A120 unreliable: see flush_packet in SendMobHeartbeat for rationale.
		// SendApp omits dbASQ_low when !ack_req to match EQClassic's wire
		// format; see [[project-trilogy-unreliable-a120-wire-format]].
		m_tzs->SendToSession(m_session_key, 0xa120, pkt, plen, /*ack_req=*/false);
	}

	m_pending_mob_updates.clear();
}

// ============================================================
// SendForcedSelfPositionUpdate — server → self OP_MobUpdate (0xa120).
//
// The v29c client normally owns its own position and rejects self-echoes
// (see HandleClientUpdate self-branch).  During SE_Fear the server is the
// movement authority: AI_Start freezes local controls and MovementManager
// steps the player toward m_FearWalkTarget each AI_movement_timer tick.
// Without a wire push, the frozen client stays visually stationary while
// the server-side position drifts — the "feared but stunned in place"
// symptom.
//
// Wire opcode is A120 (OP_MobUpdate), NOT F320 (OP_ClientUpdate) — that
// distinction matters and is why the earlier F320 attempt caused wall
// clipping + jitter.  F320 to self is a hard XY position correction, so
// each push teleports the player past client-side collision.  A120 tells
// the client "this is where this mob is + what animation it's playing";
// v29c then renders the run cycle with LOCAL COLLISION APPLIED, honoring
// walls and terrain.
//
// EQClassic proof at Zone/Source/mob.cpp:547-562 (SendPosUpdate) +
// SpellEffects.cpp:222-237 (SE_Fear branch calls it once via
// GetFearDestination).  EQClassic sends ONE A120 per fear leg — the
// client extrapolates the animation between legs from heading + anim_type.
// That's why there is no tick heartbeat here; MoveToCommand's natural
// start / speed-change / 5s cadence provides the leg-boundary triggers.
//
// Z encoding is *10 like every other outbound Z in this layer — see the
// inline comment on upd.z_pos below.
// ============================================================
void TrilogyClient::SendForcedSelfPositionUpdate(
	const PlayerPositionUpdateServer_Struct* p)
{
	if (!p) return;

	Trilogy::structs::SpawnPositionUpdate_Struct upd{};
	upd.spawn_id      = static_cast<int16_t>(TranslateId(GetID()));
	upd.heading       = static_cast<int8_t>(static_cast<uint8_t>(GetHeading() / 2.0f));
	upd.delta_heading = 0;
	upd.y_pos         = static_cast<int16_t>(GetY());
	upd.x_pos         = static_cast<int16_t>(GetX());
	// Z encoding: *10 matches EVERY OTHER outbound Z in our Trilogy layer
	// (see trilogy_zone.cpp:3497, 7671, 7810, 7878, 8033, 9208 etc.) and
	// pairs with the inbound decode at trilogy_zone.cpp:4945 (`z_pos / 10`).
	// EQClassic's mob.cpp:577 "raw for IsClient" is a server-internal Z
	// scale quirk of theirs (Yeahlight-ism), not a universal wire property.
	// Earlier attempt at raw Z made the player "fall from sky" — decoded Z
	// landed 10× above actual.
	upd.z_pos         = static_cast<int16_t>(GetZ() * 10.0f);

	// Encode fear speed as anim_type so the client renders the run cycle
	// while extrapolating position.  EncodeTrilogyAnim converts EQEmu-speed
	// to v29c wire byte; matches how NPC updates encode running speed.
	upd.anim_type = TrilogyZoneServer::EncodeTrilogyAnim(
		this, static_cast<int>(p->animation));

	// Deltas: EQClassic sends 0 for all entities in practice (see
	// trilogy_zone.cpp:395-407 for the analysis of their /125 rounding).
	// The client extrapolates from heading × anim, not from delta vectors.
	TrilogyZoneServer::EncodeTrilogyDelta(&upd, 0, 0, 0);

	m_tzs->SendToSession(m_session_key, 0xa120,
	                     BuildSingleA120Payload(upd).data(),
	                     4 + sizeof(upd),
	                     /*ack_req=*/false);

	// Log only on transition to avoid flooding the zone log across a long
	// fear cast.  first_push == fear just started or previously reset.
	const bool first_push      = (m_last_fear_self_push_ms == 0);
	const bool anim_transition = (upd.anim_type != m_last_fear_self_anim);
	if (first_push || anim_transition) {
		LogInfo("[TrilogyFear] A120 self-push sid={} pos=({:.1f},{:.1f},{:.1f}) "
		        "heading={:.1f} anim={} char=[{}] first={} trans={}",
		        upd.spawn_id, GetX(), GetY(), GetZ(), GetHeading(),
		        static_cast<int>(upd.anim_type),
		        GetCleanName(),
		        first_push ? 1 : 0, anim_transition ? 1 : 0);
	}

	m_last_fear_self_push_ms  = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	m_last_fear_self_anim = upd.anim_type;
}

// A120 wire is [int32 num_updates][SpawnPositionUpdate_Struct × N].
// For a single-mob self-push we build a 19-byte payload (4 + 15).
std::array<uint8_t, 4 + sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)>
TrilogyClient::BuildSingleA120Payload(
	const Trilogy::structs::SpawnPositionUpdate_Struct& upd)
{
	std::array<uint8_t, 4 + sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)> pkt{};
	int32_t n = 1;
	std::memcpy(pkt.data(),     &n,   4);
	std::memcpy(pkt.data() + 4, &upd, sizeof(upd));
	return pkt;
}

// ============================================================
// MaybeSendFearHeartbeat — kept as a no-op stub; hooks in
// TrilogyZoneServer::Tick still call this but EQClassic parity means
// there's nothing to do per-tick.  Fear position pushes are driven
// entirely by MoveToCommand's start / speed-change / 5s cadence via
// HandleClientUpdate's self-branch.  The v29c client extrapolates
// heading × anim_type between packets.  The earlier tick heartbeat
// was actively causing jitter by over-correcting.
// ============================================================
void TrilogyClient::MaybeSendFearHeartbeat()
{
	if (!IsFeared()) {
		if (m_last_fear_self_push_ms != 0) {
			m_last_fear_self_push_ms = 0;
			m_last_fear_self_anim    = 0;
		}
	}
}

// ============================================================
// OnClientReady — called by TrilogyZoneServer on the client's
// first ZN_OP_ClientUpdate, signalling the 3D world is up.
// Clears the zoning flag and flushes any buffered spawn/ground
// packets that were held back during zone-in or zone-out.
// ============================================================
void TrilogyClient::OnClientReady()
{
	LogInfo("[TrilogyClient] OnClientReady: flushing {} deferred spawn(s), {} deferred player/bot spawn(s)",
	        m_deferred_spawns.size(), m_deferred_player_spawns.size());
	m_is_zoning = false;
	for (auto& [opcode, data] : m_deferred_spawns)
		m_tzs->SendToSession(m_session_key, opcode, data.data(), static_cast<uint32_t>(data.size()));
	m_deferred_spawns.clear();

	// Re-resolve each deferred Client/Bot/Playerbot entity by ID and dispatch
	// the permanent-spawn multi-packet sequence.  Entities that despawned in
	// the meantime (left the zone, died, were removed) silently skip.
	for (uint16_t sid : m_deferred_player_spawns) {
		Mob* mob = entity_list.GetMob(sid);
		if (!mob) continue;
		if (mob->IsClient()) {
			m_tzs->SendPlayerSpawnPermanent(m_session_key, mob->CastToClient());
		} else if (mob->IsBot() ||
		           (mob->IsNPC() && mob->CastToNPC()->GetNPCTypeID() ==
		                            static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId)))) {
			m_tzs->SendPlayerbotSpawnPermanent(m_session_key, mob->CastToNPC());
		}
	}
	m_deferred_player_spawns.clear();
}

// ============================================================
// TrilogyPositionUpdate — called from TrilogyZoneServer when
// the client sends a 0xF320 ClientUpdate datagram.
//
// GMMove updates the entity position in the EQEmu world so that:
//   - NPC aggro / proximity scanning sees the correct position
//   - Titanium clients receive a mob-update broadcast
// ============================================================

void TrilogyClient::TrilogyPositionUpdate(float x, float y, float z, float heading)
{
	// Capture the prior x/y BEFORE SetPosition so the moving flag can be derived
	// from the actual delta — matching the modern Client::Handle_OP_ClientUpdate
	// (client_packet.cpp:5013).  Without this, IsMoving() stays false for the
	// entire session and every passive skill that gates on movement (swimming,
	// mob-close aggro scan cadence, etc.) never fires.
	const float prev_x = GetX();
	const float prev_y = GetY();

	// GMMove would crash inside MobMovementManager::FillCommandStruct when it
	// tries to broadcast position to clients (mob->IsBot() on TrilogyClient
	// triggers an access violation in the movement manager).
	// SetPosition + SetHeading update the mob's world position for NPC aggro
	// and proximity checks without triggering the movement manager broadcast.
	SetPosition(x, y, z);
	SetHeading(heading);
	SetMoving(!(x == prev_x && y == prev_y));

	// Proximity events — mirrors modern Handle_OP_ClientUpdate
	// (client_packet.cpp:4996-5002).  Without this call, EVENT_ENTER /
	// EVENT_EXIT on NPC proximity boxes (quest::set_proximity), the area
	// EVENT_ENTER_AREA / EVENT_LEAVE_AREA path, and task-system proximity
	// objectives never observe player movement on Trilogy clients — the
	// geometry check itself is client-agnostic, it just needs the position
	// stream to reach it.  Timer + m_Proximity update match the modern block
	// exactly so first-tick behavior matches too.
	if (proximity_timer.Check()) {
		entity_list.ProcessMove(this, glm::vec3(x, y, z));
		if (RuleB(TaskSystem, EnableTaskSystem) && RuleB(TaskSystem, EnableTaskProximity))
			ProcessTaskProximities(x, y, z);
		m_Proximity = glm::vec3(x, y, z);
	}

	// Hide-break on movement — mirrors modern Handle_OP_ClientUpdate
	// (client_packet.cpp:4995-5011).  The proxy bypasses that handler entirely,
	// so without this check `hidden` / `improved_hidden` persisted across every
	// position update.  Effect: a non-sneaking rogue who pressed Hide once
	// stayed server-side hidden forever, bypassing NPC aggro until they
	// re-pressed Hide or zoned — a real stealth exploit, not just a visual gap.
	//
	// The sneak+hide combo (any class) intentionally falls through this guard:
	// `(hidden) && !sneaking` keeps hidden true when both flags are set, which
	// is the correct modern rogue mechanic.  Whether the v29c client renders
	// the player as invisible while moving is a client-side decision the
	// proxy cannot reach — but NPC aggro logic on the server reads `hidden`
	// directly, so the gameplay effect (walking past a KOS NPC undetected)
	// works regardless of what the local render shows.
	if (IsMoving() && (hidden || improved_hidden) && !sneaking) {
		hidden = false;
		improved_hidden = false;
		// Only broadcast the visibility change if the player wasn't already
		// spell-invisible — otherwise the spell would lose its invis render to
		// other observers prematurely.
		if (!invisible) {
			auto outapp = new EQApplicationPacket(
			    OP_SpawnAppearance, sizeof(::SpawnAppearance_Struct));
			auto* sa_out = reinterpret_cast<::SpawnAppearance_Struct*>(outapp->pBuffer);
			sa_out->spawn_id  = GetID();
			sa_out->type      = AppearanceType::Invisibility; // 0x03
			sa_out->parameter = 0; // visible
			entity_list.QueueClients(this, outapp, true); // ignore_self
			safe_delete(outapp);
		}
	}

	// Keep m_pp in sync so SaveCharacterData writes the current position on disconnect.
	GetPP().x       = x;
	GetPP().y       = y;
	GetPP().z       = z;
	GetPP().heading = heading;

	// Trilogy clients never receive OP_ZonePoints data, so zone-line detection
	// happens server-side per position update.
	//
	// Virtual zone lines (rectangular AABB triggers on zone_points.is_virtual=1)
	// stay on the modern EQEmu path — those are typically GM-authored
	// teleporters and coexist with EQClassic content.
	//
	// Server-side classic zoneline detection: prefer the EQClassic-parity path
	// (CheckTrilogyZoneLines against trilogy_zone_line_list, imported from
	// EQClassic). When a zone has NO trilogy_zone_points entries loaded, fall
	// back to the legacy sphere+wildcard path (CheckTraditionalZonePoints
	// against zone_point_list) so Trilogy players in non-imported zones aren't
	// stranded. Coverage today is Classic/Kunark/Velious (~121 zones); other
	// zones use the sphere fallback until they're imported.
	CheckVirtualZoneLines();
	if (zone && !zone->trilogy_zone_line_list.empty()) {
		CheckTrilogyZoneLines();
	}
	else {
		CheckTraditionalZonePoints();
	}

	// Swimming skill-up + water-region transition.  Mirrors the watermap block
	// in modern Client::Handle_OP_ClientUpdate (client_packet.cpp:5159-5170) —
	// the proxy never enters that handler, so every gameplay effect that hangs
	// off the watermap (swim skill-up, breath-timer reset on entering/leaving
	// liquid, horse auto-dismount in water) was dead.
	if (zone->watermap) {
		if (zone->watermap->InLiquid(glm::vec3(GetPosition())) && IsMoving()) {
			CheckIncreaseSkill(EQ::skills::SkillSwimming, nullptr, -17);

			if (GetHorseId() && RuleB(Character, DismountWater)) {
				SetHorseId(0);
				BuffFadeByEffect(SE_SummonHorse);
			}
		}
		CheckRegionTypeChanges();
	}
}

// ============================================================
// CheckTrilogyZoneLines — EQClassic-parity server-side zone-line detection.
//
// Faithful port of EQClassic's Client::ScanForZoneLines
// (EQClassic/Zone/Source/client.cpp:3780+). Iterates the imported
// zone->trilogy_zone_line_list and honors all three modes:
//   UseNewZoning == 0 : old-mode box detection with Z tolerance + eye-level
//                       guard + mandatory LOS.
//   UseNewZoning == 1 : X-based plane crossing with MinVert/MaxVert Z-wall.
//   UseNewZoning == 2 : Y-based plane crossing, same idea.
//
// On fire, computes the destination via EQClassic's keepX/Y/Z + preloaded
// dest_CenterPoint/dest_MinVert/dest_MaxVert remap, stores it in
// m_ZoneSummonLocation, sets m_trilogy_use_eqclassic_dest so
// Handle_OP_ZoneChange takes it verbatim (no wildcard/delta math), and sends
// OP_RequestClientZoneChange. The client's OP_ZoneChange reply then flows
// through the existing HandleZoneChange -> Handle_OP_ZoneChange -> DoZoneSuccess
// chain unchanged.
//
// DB heading is stored in EQClassic's 0-255 wire scale; we multiply by 2 at
// fire time to convert to EQEmu's 0-512 internal scale. TrilogyClient's
// outbound heading encoders then divide by 2 back to wire for the client.
//
// Explicitly NOT ported from EQClassic:
//   - Cross-zone FindBestZ for arrival Z (documented DEAD END in memory
//     project_trilogy_zone_transition — the departing zone process has not
//     loaded the destination zone's collision map). We pass raw target_z
//     through; destination-side SendPlayerProfile terrain-snap handles bad Z.
//   - LOS check on plane-crossing modes (EQClassic doesn't do it there either).
//   - Same-zone teleport special case (EQClassic distinguishes; not needed
//     here — firing OP_RequestClientZoneChange to the same zone id still
//     works end-to-end via the standard ZoneChange machinery).
// ============================================================
void TrilogyClient::CheckTrilogyZoneLines()
{
	// Same gate as legacy CheckTraditionalZonePoints: only fire during idle,
	// non-zoning state; don't spam OP_RequestClientZoneChange while a zoning
	// handshake is already in flight.
	if (!zone || zone_mode != ZoneUnsolicited || bZoning) {
		return;
	}
	// Strict EQClassic parity: zones without imported content get NO server-side
	// detection here. Any Trilogy player in such a zone can still zone via
	// client-autonomous OP_ZoneChange (outdoor zonelines) but nothing fires
	// from this path.
	if (zone->trilogy_zone_line_list.empty()) {
		return;
	}

	const bool zp_dbg = RuleB(Zone, TrilogyZonePointDebug);

	// Zone-in loop guard: same shape as CheckTraditionalZonePoints — suppress
	// detection until player has moved 2*guard_r from spawn (see
	// ArmTrilogyZoneInGuard for the per-line effective_r calculation).
	if (m_trilogy_zonein_guard) {
		const float gdx        = GetX() - m_trilogy_zonein_x;
		const float gdy        = GetY() - m_trilogy_zonein_y;
		const float kGuardDist = 2.0f * m_trilogy_zonein_guard_r;
		const float g_dist2    = gdx * gdx + gdy * gdy;
		if (g_dist2 < kGuardDist * kGuardDist) {
			if (zp_dbg) {
				LogInfo("[TrilogyZP DBG] guard ACTIVE (trilogy) char [{}]"
				        " | player ({:.1f},{:.1f}) spawn ({:.1f},{:.1f})"
				        " | dist={:.1f}/{:.1f}u (guard_r={:.1f})",
				        GetCleanName(), GetX(), GetY(),
				        m_trilogy_zonein_x, m_trilogy_zonein_y,
				        std::sqrt(g_dist2), kGuardDist, m_trilogy_zonein_guard_r);
			}
			return;
		}
		m_trilogy_zonein_guard = false;
		if (zp_dbg) {
			LogInfo("[TrilogyZP DBG] guard CLEARED (trilogy) char [{}]"
			        " | moved {:.1f}u from spawn (threshold {:.1f}u)",
			        GetCleanName(), std::sqrt(g_dist2), kGuardDist);
		}
	}

	// Per-tick iteration — first line that fires wins (matches EQClassic's
	// `break` at the fire point).
	for (const auto &zln : zone->trilogy_zone_line_list) {
		bool  fired        = false;
		float dest_x       = 0.0f;
		float dest_y       = 0.0f;
		float dest_z       = 0.0f;
		float dest_heading = zln.heading * 2.0f; // EQClassic 0-255 -> EQEmu 0-512
		const char* mode_tag = "?";

		if (zln.UseNewZoning == 0) {
			// --- OLD-MODE BOX (~96% of the imported dataset) ---
			mode_tag = "box";
			const int32 zRangeVal = zln.Zrange;
			const int32 zDiffVal  = (zln.maxZDiff == 0) ? 50000 : zln.maxZDiff;

			// Wildcard XY support — mirrors modern EQEmu's CheckTraditionalZonePoints
			// wildcard sentinel (client.cpp:9783). Rows with x == +/-999999 or
			// y == +/-999999 skip the corresponding axis in the box test.
			//
			// Primary use: fall-through zone triggers (e.g. Plane of Sky
			// row: x/y wildcard, z=-2000 -> player falling off the islands
			// zones to freporte docks once Z drops below the threshold).
			// Without wildcards, that mechanism couldn't exist in Trilogy —
			// the box test would always fail on x=999999 vs any real player X.
			//
			// The LOS check + eye-level guard are also skipped for wildcard-
			// XY rows: there's no "trigger point" to raycast from or gate
			// on (the trigger is effectively the whole Z plane). Matches
			// modern EQEmu behavior which only applies those guards to
			// non-wildcard entries.
			const bool xWild = (std::fabs(zln.x) >= 999998.0f);
			const bool yWild = (std::fabs(zln.y) >= 999998.0f);

			if (!xWild && std::fabs(GetX() - zln.x) > zRangeVal) continue;
			if (!yWild && std::fabs(GetY() - zln.y) > zRangeVal) continue;
			if (std::fabs(GetZ() - zln.z) > zDiffVal) continue;
			if (!xWild && !yWild) {
				// Eye-level guard: prevent triggering when the player's
				// head-level (+10u above their feet) is still below the
				// trigger's floor. Only meaningful for real spatial triggers.
				if ((GetZ() + 10.0f) < zln.z) continue;

				// Mandatory LOS in EQClassic old-mode. Fail-open when the
				// zone's map isn't loaded (Mob::CheckLosFN returns false on
				// null zonemap in this build), so a map-less zone still
				// gets detection.
				if (zone->zonemap && !CheckLosFN(zln.x, zln.y, zln.z, 5.0f)) {
					if (zp_dbg) {
						LogInfo("[TrilogyZP DBG] trilogy box zp id={} LOS BLOCK"
						        " | player ({:.1f},{:.1f},{:.1f}) trig ({:.1f},{:.1f},{:.1f})",
						        zln.id, GetX(), GetY(), GetZ(),
						        zln.x, zln.y, zln.z);
					}
					continue;
				}
			}

			// Fire: keepX/Y/Z overrides the corresponding coord with the
			// player's current position (EQClassic content authors set these
			// only where zones' axes line up so this is a safe pass-through).
			//
			// Also treat target coord == +/-999999 as an implicit "keep" flag:
			// modern EQEmu uses that sentinel to mean "preserve this axis
			// across the crossing" (zone_points.target_x/y wildcards for
			// outdoor seamless transitions, and dungeon rows where a specific
			// axis carries over between adjacent maps). Many trilogy_zone_points
			// rows have e.g. `target_y=999999` set without also setting
			// `keepY=1` explicitly — prior to this change the engine literally
			// teleported the player to Y=999999 (bogus coord). Recognizing the
			// sentinel makes those rows self-consistent without also toggling
			// the keep flag. Sentinel threshold 999998 is well beyond any
			// legitimate EQ coord (zones range roughly ±3000).
			const bool keepX_effective = (zln.keepX == 1) || (std::fabs(zln.target_x) >= 999998.0f);
			const bool keepY_effective = (zln.keepY == 1) || (std::fabs(zln.target_y) >= 999998.0f);
			const bool keepZ_effective = (zln.keepZ == 1) || (std::fabs(zln.target_z) >= 999998.0f);
			dest_x = keepX_effective ? GetX() : zln.target_x;
			dest_y = keepY_effective ? GetY() : zln.target_y;
			dest_z = keepZ_effective ? GetZ() : zln.target_z;
			fired  = true;
		}
		else if (zln.UseNewZoning == 1) {
			// --- X-based PLANE CROSSING ---
			mode_tag = "planeX";
			const float triggerX = zln.x;
			const float zwallMin = (zln.MinVert == 0.0f) ? -999999.0f : zln.MinVert;
			const float zwallMax = (zln.MaxVert == 0.0f) ?  999999.0f : zln.MaxVert;

			bool crossed = false;
			// EQClassic uses two INDEPENDENT branches (>= and <=) selected by
			// the sign of triggerX. A triggerX of exactly 0 satisfies both,
			// a mild edge case in their code we replicate faithfully.
			if (triggerX >= 0.0f) {
				if (GetX() >= triggerX && GetY() >= zwallMin && GetY() <= zwallMax) {
					crossed = true;
				}
			}
			if (!crossed && triggerX <= 0.0f) {
				if (GetX() <= triggerX && GetY() >= zwallMin && GetY() <= zwallMax) {
					crossed = true;
				}
			}
			if (!crossed) continue;

			// Destination X = raw target_x. Destination Y uses keepY logic:
			//   keepY == 1  -> preserve player's Y (clamped to destination Min/Max)
			//   keepY == 0  -> centerpoint-relative remap using the paired
			//                  destination line's CenterPoint/Min/Max (preloaded
			//                  at LoadTrilogyZonePoints). If the paired lookup
			//                  didn't resolve, fall back to raw target_y.
			dest_x = zln.target_x;
			float sendY;
			if (zln.keepY == 1) {
				sendY = GetY();
				if (zln.dest_resolved &&
				    zln.dest_MaxVert != 0.0f && zln.dest_MinVert != 0.0f)
				{
					if (sendY > zln.dest_MaxVert) sendY = zln.dest_MaxVert;
					if (sendY < zln.dest_MinVert) sendY = zln.dest_MinVert;
				}
			}
			else {
				if (zln.dest_resolved) {
					const float dist_from_center = GetY() - zln.CenterPoint;
					sendY = zln.dest_CenterPoint + dist_from_center;
					if (sendY > zln.dest_MaxVert) sendY = zln.dest_MaxVert;
					if (sendY < zln.dest_MinVert) sendY = zln.dest_MinVert;
				}
				else {
					sendY = zln.target_y;
				}
			}
			dest_y = sendY;
			// Z: raw target_z. Cross-zone FindBestZ is not viable here (see
			// module comment). Destination-side terrain-snap catches bad Z.
			dest_z = zln.target_z;
			fired  = true;
		}
		else if (zln.UseNewZoning == 2) {
			// --- Y-based PLANE CROSSING (X/Y roles swapped from planeX) ---
			mode_tag = "planeY";
			const float triggerY = zln.y;
			const float zwallMin = (zln.MinVert == 0.0f) ? -999999.0f : zln.MinVert;
			const float zwallMax = (zln.MaxVert == 0.0f) ?  999999.0f : zln.MaxVert;

			bool crossed = false;
			if (triggerY >= 0.0f) {
				if (GetY() >= triggerY && GetX() >= zwallMin && GetX() <= zwallMax) {
					crossed = true;
				}
			}
			if (!crossed && triggerY <= 0.0f) {
				if (GetY() <= triggerY && GetX() >= zwallMin && GetX() <= zwallMax) {
					crossed = true;
				}
			}
			if (!crossed) continue;

			dest_y = zln.target_y;
			float sendX;
			if (zln.keepX == 1) {
				sendX = GetX();
				if (zln.dest_resolved &&
				    zln.dest_MaxVert != 0.0f && zln.dest_MinVert != 0.0f)
				{
					if (sendX > zln.dest_MaxVert) sendX = zln.dest_MaxVert;
					if (sendX < zln.dest_MinVert) sendX = zln.dest_MinVert;
				}
			}
			else {
				if (zln.dest_resolved) {
					const float dist_from_center = GetX() - zln.CenterPoint;
					sendX = zln.dest_CenterPoint + dist_from_center;
					if (sendX > zln.dest_MaxVert) sendX = zln.dest_MaxVert;
					if (sendX < zln.dest_MinVert) sendX = zln.dest_MinVert;
				}
				else {
					sendX = zln.target_x;
				}
			}
			dest_x = sendX;
			dest_z = zln.target_z;
			fired  = true;
		}
		// Any UseNewZoning value outside {0, 1, 2} is data corruption; skip.

		if (!fired) continue;

		LogInfo(
			"[TrilogyZP] CheckTrilogyZoneLines FIRED: char [{}] zone [{}]"
			" | line id={} mode={} player ({:.1f},{:.1f},{:.1f})"
			" trig ({:.1f},{:.1f},{:.1f})"
			" -> dest ({:.2f},{:.2f},{:.2f},{:.1f}) target_zone={}"
			" keep [{}{}{}]",
			GetCleanName(), zone->GetShortName(),
			zln.id, mode_tag,
			GetX(), GetY(), GetZ(),
			zln.x, zln.y, zln.z,
			dest_x, dest_y, dest_z, dest_heading, zln.target_zone_id,
			zln.keepX ? 'X' : '.', zln.keepY ? 'Y' : '.', zln.keepZ ? 'Z' : '.'
		);

		// Same-zone teleporter fast path (skyshrine pads, erudin pads, guk
		// tunnels, etc.). Use OP_TeleportPC (0x4d21) with the destination
		// zone-name equal to the current zone. v29c handles this as a
		// LOCAL intra-zone teleport (no OP_ZoneChange round-trip, no zone
		// re-connect) and — crucially — accepts the destination Z
		// authoritatively.
		//
		// Prior implementation used a forced OP_ClientUpdate (0xf320) which
		// the v29c client treats as an XY-only position correction: it
		// rejected any large Z delta and kept the pre-teleport Z, causing
		// the player to fall from height when a pad crossed between the
		// upper/lower Skyshrine floors (198 <-> 378). OP_TeleportPC's
		// "hard-teleport" semantics avoid that entirely — verified via
		// same-zone #goto tests where the client landed at the specified Z
		// without any physics settle. Arm the zone-in guard so we don't
		// immediately re-trigger the arrival pad or the return pad.
		if (zln.target_zone_id == zone->GetZoneID()) {
			// Parity with ProcessMovePC's in-zone path: drop any dragged
			// corpses and move any pet along with us. Skipping either would
			// leave state behind that a normal in-zone summon/gate wouldn't.
			ClearDraggedCorpses();
			if (GetPetID() != 0) {
				if (Mob* p = GetPet()) {
					p->SetPetOrder(SPO_Follow);
					// Offset slightly so the pet doesn't stack exactly on us
					// (mirrors ProcessMovePC's `x + 15` for the same reason).
					p->GMMove(dest_x + 15.0f, dest_y, dest_z);
				}
			}

			// Update server-side position first so any concurrent handlers
			// (aggro scan, position save on subsequent tick) see the new
			// coord immediately, independent of when the client acks.
			m_Position.x = dest_x;
			m_Position.y = dest_y;
			m_Position.z = dest_z;
			SetHeading(dest_heading);
			mMovementManager->SendCommandToClients(
				this, 0.0, 0.0, 0.0, 0.0, 0, ClientRangeAny);

			// Send OP_TeleportPC with current zone's shortname. The client
			// keys off zone_name to decide "hard teleport" (same-zone) vs
			// "zone change" (different zone); passing the current zone here
			// gives us the hard-teleport path.
			const char* zname = ZoneName(static_cast<uint32_t>(zone->GetZoneID()));
			if (zname) {
				Trilogy::structs::TeleportPC_Struct tpc{};
				memset(&tpc, 0, sizeof(tpc));
				strncpy(tpc.zone, zname, sizeof(tpc.zone) - 1);
				tpc.yPos    = dest_y;
				tpc.xPos    = dest_x;
				// Same zPos==0 workaround as the OP_RequestClientZoneChange
				// translator: the v29c client sometimes picks a random
				// location for a same-zone teleport when zPos is exactly 0
				// (see EQClassic TeleportPC comment), so nudge to 0.1.
				tpc.zPos    = (dest_z == 0.0f) ? 0.1f : dest_z;
				tpc.heading = dest_heading;
				m_tzs->SendToSession(
					m_session_key, 0x4d21,
					reinterpret_cast<const uint8_t*>(&tpc), sizeof(tpc));
			}

			ArmTrilogyZoneInGuard(dest_x, dest_y);

			LogInfo(
				"[TrilogyZP] Same-zone teleport applied (0x4d21): char [{}]"
				" line id={} -> ({:.2f},{:.2f},{:.2f}) heading={:.1f}",
				GetCleanName(), zln.id,
				dest_x, dest_y, dest_z, dest_heading
			);

			return; // first fire wins
		}

		// Set up ZoneSolicited state. m_trilogy_use_eqclassic_dest tells
		// Handle_OP_ZoneChange to use m_ZoneSummonLocation verbatim (no
		// wildcard/delta/anti-bounce math) — the destination is already final.
		zone_mode                     = ZoneSolicited;
		zonesummon_id                 = zln.target_zone_id;
		m_ZoneSummonLocation          = glm::vec4(dest_x, dest_y, dest_z, dest_heading);
		zonesummon_ignorerestrictions = 0;
		m_trilogy_use_eqclassic_dest  = true;
		// Wide/narrow classification for downstream (DoZoneSuccess sign-encodes
		// the persisted heading; destination zone reads sign to gate the
		// wide-boundary terrain-snap + SpawnCorrect trap):
		//   UseNewZoning == 0 (old-mode box) : dungeon doors / gates / corridor
		//        mouths — dest coords are DB-authored static points landing on
		//        well-defined dungeon floors. NARROW: no terrain-snap needed,
		//        no SpawnCorrect override needed.
		//   UseNewZoning >= 1 (plane crossing): seamless outdoor transitions
		//        (commons<->ecommons, karana<->EC style). The DB target_z is a
		//        reference not a walkable Y, and destination Z at the slid
		//        arrival XY isn't known here (no cross-zone FindBestZ). WIDE
		//        so the destination-side terrain-snap fires + the SpawnCorrect
		//        trap preserves the pre-computed arrival heading.
		m_trilogy_wide_boundary       = (zln.UseNewZoning != 0);

		auto* rc_app = new EQApplicationPacket(
		    OP_RequestClientZoneChange, sizeof(RequestClientZoneChange_Struct));
		auto* rc     = reinterpret_cast<RequestClientZoneChange_Struct*>(rc_app->pBuffer);
		rc->zone_id     = zln.target_zone_id;
		rc->instance_id = 0;
		rc->x           = dest_x;
		rc->y           = dest_y;
		rc->z           = dest_z;
		rc->heading     = dest_heading;
		rc->type        = 0x01;
		FastQueuePacket(&rc_app);

		return; // first fire wins — matches EQClassic's post-fire break
	}
}

// ============================================================
// StripSayLinks — remove SayLink markup for Trilogy clients.
//
// SayLink wire format: '\x12' + <body: EQ::constants::SAY_LINK_BODY_SIZE bytes>
//                      + <display text> + '\x12'
// Trilogy has no SayLink support — discard the opaque body and keep
// only the human-readable display text.
// ============================================================

static std::string StripSayLinks(const char* msg, size_t len)
{
	// Fast path: no SayLink delimiters present.
	if (!memchr(msg, '\x12', len))
		return std::string(msg, len);

	std::string out;
	out.reserve(len);

	for (size_t i = 0; i < len; ) {
		if (static_cast<unsigned char>(msg[i]) == 0x12) {
			++i; // skip opener \x12
			// Skip fixed-size link body.
			i += EQ::constants::SAY_LINK_BODY_SIZE;
			// Append display text until closing \x12 or end of buffer.
			while (i < len && static_cast<unsigned char>(msg[i]) != 0x12)
				out += msg[i++];
			if (i < len) ++i; // skip closer \x12
		} else {
			out += msg[i++];
		}
	}
	return out;
}

// ============================================================
// HandleOutgoingChannelMessage — OP_ChannelMessage (server → client)
//
// EQEmu internal ChannelMessage_Struct uses 64-byte name fields and
// uint32 language/chan_num.  Trilogy wire format uses smaller fields
// and int16 for both.  Translate and send opcode 0x0721.
// ============================================================

void TrilogyClient::HandleOutgoingChannelMessage(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::ChannelMessage_Struct)) return;

	const auto* cm_in = reinterpret_cast<const ::ChannelMessage_Struct*>(app->pBuffer);

	// Strip SayLinks from message tail before forwarding to Trilogy client.
	uint32_t msg_offset = static_cast<uint32_t>(sizeof(::ChannelMessage_Struct));
	std::string msg_str;
	if (app->size > msg_offset) {
		const char* raw = reinterpret_cast<const char*>(app->pBuffer + msg_offset);
		msg_str = StripSayLinks(raw, app->size - msg_offset);
	}
	if (msg_str.empty() || msg_str.back() != '\0') msg_str += '\0';

	uint32_t out_size = static_cast<uint32_t>(sizeof(Trilogy::structs::ChannelMessage_Struct))
	                    + static_cast<uint32_t>(msg_str.size());
	auto* buf = new uint8_t[out_size]();
	auto* cm_out = reinterpret_cast<Trilogy::structs::ChannelMessage_Struct*>(buf);

	strncpy(cm_out->targetname, cm_in->targetname, sizeof(cm_out->targetname) - 1);
	strncpy(cm_out->sender,     cm_in->sender,     sizeof(cm_out->sender)     - 1);
	cm_out->language       = static_cast<int16_t>(cm_in->language);
	cm_out->chan_num        = static_cast<int16_t>(cm_in->chan_num);
	cm_out->cm_unknown4[0] = static_cast<int8_t>(0xFF); // language skill masking per EQClassic server
	cm_out->cm_unknown4[1] = static_cast<int8_t>(0xFF);

	memcpy(buf + sizeof(Trilogy::structs::ChannelMessage_Struct),
	       msg_str.data(), msg_str.size());

	m_tzs->SendToSession(m_session_key, 0x0721, buf, out_size);
	delete[] buf;
}

// ============================================================
// HandleOutgoingWearChange — translate EQEmu OP_WearChange (Titanium format,
// 27 bytes) to Trilogy WearChange_Struct (0x9220, 16 bytes) and send.
//
// Called whenever any mob in the zone changes its worn appearance — including
// when a Trilogy player equips/unequips (via WearChange() in the zone server)
// and when a Titanium player equips.  This ensures all Trilogy clients see
// appearance changes regardless of the source client type.
// ============================================================

void TrilogyClient::HandleOutgoingWearChange(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::WearChange_Struct)) return;

	const auto* src = reinterpret_cast<const ::WearChange_Struct*>(app->pBuffer);

	// EQClassic's ProcessOP_WearChange uses QueueClients(this, pApp, ignore_sender=true).
	// Mob::WearChange() uses ignore_sender=false (default), so OP_WearChange for our own
	// appearance change comes back here.  Sending 0x9220 to the Trilogy client for its
	// own equipment change causes a feedback loop — the client re-sends 0x9220.
	if (src->spawn_id == GetID()) return;

	// EQClassic-faithful WearChange policy: emit reliably only when the
	// material actually changes from what the v29c client last rendered.
	// EQClassic enforces this implicitly by mutating its Mob `equipment[]`
	// arrays and firing WearChange only at the mutation point.  EQEmu's
	// philosophy is the opposite — re-broadcast current state on
	// CompleteConnect, illusion, pet summon, per-loot-slot, etc. — which
	// blasts 7-8 redundant WearChange per event through our reliable pipe
	// for zero rendering benefit (the client already has the value).
	//
	// We bridge by holding the authoritative model of the client's known
	// material per (spawn_id, slot) in m_client_known_material.  Initial
	// values are seeded at spawn-build time from sp.equipment[slot] — the
	// spawn packet IS the first WearChange in EQClassic terms.  At runtime,
	// any incoming WearChange that matches the cached value is a no-op
	// replay and gets suppressed; any actual mutation is sent reliably and
	// the cache is updated.
	//
	// Lossless: a suppressed packet by definition carries the same material
	// the client already rendered from the prior reliable delivery.  Visual
	// fidelity is preserved across all paths (spawn struct equipment[9],
	// per-slot mutation, unequip→0 transitions, skeleton texture variants
	// — all pass through correctly on the first change).
	const uint8_t material = static_cast<uint8_t>(src->material & 0xFF);
	const uint16_t spawn_id_w = static_cast<uint16_t>(src->spawn_id);
	const uint8_t slot       = static_cast<uint8_t>(src->wear_slot_id);
	const uint32_t cache_key =
		(static_cast<uint32_t>(spawn_id_w) << 8) | static_cast<uint32_t>(slot);

	auto it = m_client_known_material.find(cache_key);
	if (it != m_client_known_material.end() && it->second == material) {
		return; // client already rendered this exact material — EQClassic wouldn't emit
	}
	RecordKnownMaterial(spawn_id_w, slot, material);

	using TrilWC = Trilogy::structs::WearChange_Struct;
	TrilWC wc{};
	wc.spawn_id     = static_cast<int32_t>(src->spawn_id);
	wc.wear_slot_id = static_cast<int8_t>(src->wear_slot_id);
	wc.slot_graphic = static_cast<int8_t>(material);
	wc.sub_op       = 0;
	// Strip the 0xFF000000 "no tint" sentinel that legacy items.color carries
	// (see NormalizeTintColor).  Without this leather/chain gear renders
	// pitch-black on v29c because the client applies the RGB unconditionally.
	wc.color        = static_cast<int32_t>(
		Trilogy::NormalizeTintColor(static_cast<uint32_t>(src->color.Color)));
	wc.wc_unknown3  = 0;
	wc.flag         = 0;

	m_tzs->SendToSession(m_session_key, 0x9220,
	                     reinterpret_cast<const uint8_t*>(&wc),
	                     static_cast<uint32_t>(sizeof(wc)));
}

// ============================================================
// HandleOutgoingSpawnAppearance — translate EQEmu OP_SpawnAppearance
// (8-byte ::SpawnAppearance_Struct) to Trilogy 0xf520 (12 bytes) and send.
//
// Fires when ANY mob in the zone changes appearance state (sit/stand/anon/AFK/
// invisibility/etc.) — needed so this Trilogy client sees other players' state
// transitions. The player's own change is broadcast with ignore_self=true by
// Handle_OP_SpawnAppearance, so we don't usually echo back, but we still guard
// against feedback loops by spawn_id since the type=0x10 self-id packet uses
// spawn_id=0 and would harmlessly skip the check.
// ============================================================

void TrilogyClient::HandleOutgoingSpawnAppearance(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::SpawnAppearance_Struct)) return;
	const auto* src = reinterpret_cast<const ::SpawnAppearance_Struct*>(app->pBuffer);

	// Self-id filter: only drop Animation (sit/stand) echoes — the client
	// animates locally on the button press and a server echo would cause a
	// double-animate.  ALL other self-targeted state updates must reach the
	// v29c client, because they drive local rendering and combo logic that
	// the client cannot derive on its own:
	//
	//   Sneak (15)        — Handle_OP_Sneak QueuePacket → self.  Without
	//                       this, the v29c client never knows the server
	//                       thinks it's sneaking, so the rogue sneak+hide
	//                       combo (move while invisible) never engages —
	//                       movement locally breaks the hide render.
	//                       EQClassic intentionally sends this to self via
	//                       SendAppearancePacket(..., SAT_Sneaking, ..., true)
	//                       at client_process.cpp:5472.
	//   Invisibility (3)  — spell-invis casts broadcast to all incl. self.
	//   FlyMode (19)      — levitate self-target.
	//   Light (5), etc.   — appearance state the client needs to render.
	//
	// NOTE: no self-Animation filter here.  The server already uses
	// ignore_self=true when broadcasting player-initiated sit/stand
	// (Handle_OP_SpawnAppearance line 14890), so the player never
	// receives their own echo.  The only self-targeted Animation
	// packets that reach here are from AI_Start (Freeze=102, locks
	// controls for fear/charm/mez) and AI_Stop (Standing=100, restores
	// controls) — both must be forwarded.

	Trilogy::structs::SpawnAppearance_Struct out{};
	// Trilogy entities use the spawn_id space we hand out via TranslateId.
	out.spawn_id  = static_cast<int16_t>(TranslateId(static_cast<uint32_t>(src->spawn_id)));
	out.type      = static_cast<int16_t>(src->type);
	out.parameter = static_cast<int32_t>(src->parameter);

	// Dedup: same (spawn_id, type, parameter) as the last one we already sent
	// for this spawn is pure noise — the v29c client has already applied that
	// state.  See m_last_appearance comment in the header.
	const uint16_t key = static_cast<uint16_t>(out.spawn_id);
	const auto pv = std::make_pair(out.type, out.parameter);
	auto it = m_last_appearance.find(key);
	if (it != m_last_appearance.end() && it->second == pv) {
		LogInfo("[TrilogyDiag] F520 dedup sid={} type={} param={}",
		        out.spawn_id, out.type, out.parameter);
		return;
	}
	if (m_last_appearance.size() >= kMaxAppearanceCache) {
		m_last_appearance.clear();
	}
	m_last_appearance[key] = pv;

	LogInfo("[TrilogyDiag] F520 send sid={} type={} param={}",
	        out.spawn_id, out.type, out.parameter);

	m_tzs->SendToSession(m_session_key, 0xf520,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleOutgoingSpecialMesg — OP_SpecialMesg (server → client)
//
// EQEmu uses OP_SpecialMesg (Titanium wire format) for colored text.
// EQClassic uses opcode 0x8021 with a much simpler layout:
//   int32  msg_type  — EQClassic MESSAGETYPE_* (256+)
//   char   message[] — null-terminated text
//
// EQEmu OP_SpecialMesg SerializeBuffer layout (Client::Message):
//   int8  speak_mode  [0]
//   int8  journal_mode [1]
//   int8  language     [2]
//   uint32 type        [3..6]  — Chat::* color code
//   uint32 target_id   [7..10]
//   string sender      [11..]  null-terminated (empty = 1 null byte)
//   int32  x           follows sender
//   int32  y
//   int32  z
//   string message     actual text
//
// EQEmu Chat::* ≥ 256 and EQClassic MESSAGETYPE_* share the same values.
// EQEmu Chat::* 0-255 (raw color codes) map to MESSAGETYPE_Broadcasts (269).
// ============================================================

// Map EQEmu Chat::* type to a v29c OP_SpecialMesg msg_type value.
// v29c interprets msg_type both as a raw color (0-20) AND as a real EQClassic
// MESSAGETYPE_* (256+).  EQEmu Chat::* values fall into three buckets:
//   - 0-20:    raw color codes (Chat::Yellow=15, Chat::Red=13).  Pass through;
//              v29c renders these as that color directly.
//   - 256+:    EQClassic MESSAGETYPE_* (Say/Tell/Skills/...).  Pass through;
//              v29c knows MT_Say..MT_Disciplines (256-271).  Modern EQEmu types
//              beyond that range (Chat::Experience=334, Chat::SpellFailure=289)
//              with no v29c equivalent are remapped to the matching raw color
//              via the switch.
//   - 21-255: undefined.  Default to MT_Broadcasts (269) so text stays visible.
static uint32_t ChatTypeToTrilogyMsgType(uint32_t chat_type)
{
	switch (chat_type) {
		case 334: return 15; // Chat::Experience  → YELLOW (matches EQClassic Message(15,...))
		case 289: return 13; // Chat::SpellFailure → RED
	}
	if (chat_type <= 20u)   return chat_type; // raw v29c color
	if (chat_type >= 256u)  return chat_type; // assume EQClassic MT_* compatible
	return 269u; // MESSAGETYPE_Broadcasts — visible white fallback
}

// Resolve a handful of common system string-ids (faction / experience / level) to
// their English templates so they can be relayed to the Trilogy client, which has
// no usable formatted-string path from us.  Arg-bearing ones (GAIN_LEVEL, FACTION_*)
// arrive via OP_FormattedMessage (%1..%9 filled from the message args); the no-arg
// XP ones arrive via OP_SimpleMessage.  Returns nullptr for string-ids we don't relay.
static const char* TrilogySystemStringTemplate(uint32_t string_id)
{
	switch (string_id) {
		case 138:   return "You gain experience!!";                                    // GAIN_XP
		case 139:   return "You gain party experience!!";                             // GAIN_GROUPXP
		case 447:   return "You have gained a level! Welcome to level %1!";           // GAIN_LEVEL
		case 469:   return "Your faction standing with %1 could not possibly get any worse.";
		case 470:   return "Your faction standing with %1 got worse.";
		case 471:   return "Your faction standing with %1 could not possibly get any better.";
		case 472:   return "Your faction standing with %1 got better.";
		case 5085:  return "You gained raid experience!";                             // GAIN_RAIDEXP
		case 9298:  return "You gain party experience (with a bonus)!";               // GAIN_GROUPXP_BONUS
		case 9301:  return "You gain party experience (with a penalty)!";             // GAIN_GROUPXP_PENALTY
		case 14541: return "You gain experience (with a bonus)!";                     // GAIN_XP_BONUS
		case 14542: return "You gain experience (with a penalty)!";                   // GAIN_XP_PENALTY
		// Trap skill feedback — all no-arg templates.  Without these, every
		// Disarm Traps outcome silently drops on the v29c side (success, fail,
		// out-of-range, undetected) because each path goes through
		// MessageString → OP_FormattedMessage rather than literal-text
		// Message → OP_SpecialMesg.  Sense Traps' "no trap" path uses literal
		// text and already works.
		case 305:   return "You have disarmed the trap.";                             // DISARMED_TRAP
		case 367:   return "You have not detected any traps.";                        // LDON_SENSE_TRAP2
		case 368:   return "You are too far away from that trap to affect it.";       // TRAP_TOO_FAR
		case 370:   return "You fail to disarm the detected trap.";                   // FAIL_DISARM_DETECTED_TRAP
		// Rogue-only Hide / Sneak / Evade feedback — sent by Handle_OP_Hide and
		// Handle_OP_Sneak via OP_SimpleMessage (FastQueuePacket → self).  Same
		// drop pattern as the trap strings above.  Non-Rogue classes never get
		// these so this only matters when the v29c client is a Rogue.
		case 343:   return "You have momentarily ducked away from the main combat.";  // EVADE_SUCCESS
		case 344:   return "Your attempts at ducking clear of combat fail.";          // EVADE_FAIL
		case 345:   return "You failed to hide yourself.";                            // HIDE_FAIL
		case 346:   return "You have hidden yourself from view.";                     // HIDE_SUCCESS
		case 347:   return "You are as quiet as a cat stalking its prey.";            // SNEAK_SUCCESS
		case 348:   return "You are as quiet as a herd of running elephants.";        // SNEAK_FAIL
		// Monk Mend feedback — Handle_OP_Mend MessageString paths
		// (client_packet.cpp:10449,10452,10468,10471).  Without these the
		// monk gets zero visible response on press; the HP change happens
		// silently on success / worsen.  Note: handler also silently
		// returns early when GetSkill(SkillMend)==0 (HasSkill gate), so
		// untrained monks see nothing for a separate reason — must
		// #setskill 32 N (or visit a Monk GM) first.
		case 349:   return "You magically mend your wounds and heal considerable damage."; // MEND_CRITICAL
		case 350:   return "You mend your wounds and heal some damage.";              // MEND_SUCCESS
		case 351:   return "You have worsened your wounds!";                          // MEND_WORSEN
		case 352:   return "You have failed to mend your wounds.";                    // MEND_FAIL
		// Feign Death — observer broadcast on failure (MessageCloseString from
		// Handle_OP_FeignDeath line 6542 with player name as %1).  Self gets no
		// message on success OR failure by EQ design; this only reaches NEARBY
		// v29c-connected players so they see who collapsed.
		case 1456:  return "%1 has fallen to the ground.";                            // STRING_FEIGNFAILED
		// Forage feedback — all no-arg templates fired from Client::ForageItem
		// (forage.cpp:434+) via MessageString.  Without these the v29c client
		// gets zero visible feedback on Forage success or failure (the food
		// item still lands on the cursor via the item-packet path, but the
		// success line is dropped).
		case 150:   return "You have scrounged up some fishing grubs.";               // FORAGE_GRUBS
		case 151:   return "You have scrounged up some water.";                       // FORAGE_WATER
		case 152:   return "You have scrounged up some food.";                        // FORAGE_FOOD
		case 153:   return "You have scrounged up some drink.";                       // FORAGE_DRINK
		case 154:   return "You have scrounged up something that doesn't look edible."; // FORAGE_NOEAT
		case 155:   return "You fail to locate any food nearby.";                     // FORAGE_FAILED
		case 6012:  return "Your forage mastery has enabled you to find something else!"; // FORAGE_MASTERY
		// Generic skill helpers — fire from Forage, Bind Wound and others.
		case 290:   return "Duplicate lore items are not allowed.";                   // DUP_LORE
		case 12393: return "You can not use this skill while on a mount.";            // NO_SKILL_WHILE_MOUNTED
		// Merchant / trade busy — fires from bot ^invremove (Chat::Tell) when the
		// player has an open tradeskill container or active trade window.  Without
		// this template the player gets zero feedback and assumes the command is
		// broken.
		case 1143:  return "I'm sorry, I am busy right now.";                         // MERCHANT_BUSY
		// Door / lockpick feedback — Doors::HandleClick paths.  All MessageString
		// calls, so without these the v29c client gets zero text on every locked /
		// picked / GM scenario (only the plain-text "This is locked..." at
		// doors.cpp:366 reaches it, and only when the player has nothing on
		// cursor).  Templates cover the GM-bypass success line, the
		// no-pick / wrong-pick / insufficient-skill rejections, and the
		// successful-lockpick confirmation.
		case 130:   return "It's locked and you're not holding the key.";             // DOORS_LOCKED
		case 131:   return "This lock cannot be picked.";                             // DOORS_CANT_PICK
		case 132:   return "You are not sufficiently skilled to pick this lock.";     // DOORS_INSUFFICIENT_SKILL
		case 133:   return "You opened the locked door with your magic GM key.";      // DOORS_GM
		case 1457:  return "You successfully picked the lock.";                       // DOORS_SUCCESSFUL_PICK
		case 7564:  return "You must have a lock pick in your inventory to do this."; // DOORS_NO_PICK
		// Disarm skill feedback — NPC::Disarm + Client::Disarm both fire
		// MessageString(Chat::Skills, DISARM_SUCCESS/FAILED).  Without these
		// the player sees no on-screen confirmation of the disarm attempt's
		// outcome (the WearChange to remove the weapon visual still works,
		// but the text feedback is dropped).
		case 12890: return "You disarmed %1!";                                        // DISARM_SUCCESS
		case 12891: return "Your attempt to disarm failed.";                          // DISARM_FAILED
		// Intimidation feedback — Mob::InstillDoubt fires NOT_SCARING on every
		// failed roll (and the formula's harsh, so most attempts produce this).
		// EQEmu has no corresponding success message (`// is there a success
		// message?` comment at special_attacks.cpp:2384); on success the NPC
		// just flees from the SpellOnTarget(229) fear cast.  Without this
		// allowlist entry the player presses Intimidation, sees the animation,
		// and gets zero text feedback whether it worked or not.
		case 164:   return "You're not scaring anyone.";                              // NOT_SCARING
		// Fishing feedback — CanFish() validation + GoFish() outcome MessageString
		// paths (forage.cpp:197-249, 355-408).  Without these the entire cast
		// cycle is silent on the v29c side: no equip-pole nag, no land/lava
		// rejection, no catch / miss / lost-bait line.  The successful catch
		// also has a %1 variant (421) used when the item is a food type.
		case 160:   return "You can't fish without a fishing pole, go buy one.";      // FISHING_NO_POLE
		case 161:   return "You need to put your fishing pole in your primary hand."; // FISHING_EQUIP_POLE
		case 162:   return "You can't fish without fishing bait, go buy some.";       // FISHING_NO_BAIT
		case 163:   return "You cast your line.";                                     // FISHING_CAST
		case 165:   return "You stop fishing and go on your way.";                    // FISHING_STOP
		case 166:   return "Trying to catch land sharks perhaps?";                    // FISHING_LAND
		case 167:   return "Trying to catch a fire elemental or something?";          // FISHING_LAVA
		case 168:   return "You didn't catch anything.";                              // FISHING_FAILED
		case 169:   return "Your fishing pole broke!";                                // FISHING_POLE_BROKE
		case 170:   return "You caught, something...";                                // FISHING_SUCCESS
		case 421:   return "You caught %1!";                                          // FISHING_SUCCESS_FISH_NAME
		case 171:   return "You spill your beer while bringing in your line.";        // FISHING_SPILL_BEER
		case 172:   return "You lost your bait!";                                     // FISHING_LOST_BAIT
		// Spell resist / fizzle / interrupt feedback — MessageString paths from
		// spells.cpp (ResistSpell, CheckFizzle, InterruptSpell).
		case 173:   return "Your spell fizzles!";                                     // SPELL_FIZZLE
		case 180:   return "You miss a note, bringing your song to a close!";         // MISS_NOTE
		case 425:   return "Your target resisted the %1 spell.";                      // TARGET_RESISTED
		case 426:   return "You resist the %1 spell!";                                // YOU_RESIST
		case 439:   return "Your spell is interrupted.";                              // INTERRUPT_SPELL
		case 1218:  return "%1's spell fizzles!";                                     // SPELL_FIZZLE_OTHER
		case 1219:  return "A missed note brings %1's song to a close!";              // MISSED_NOTE_OTHER
		case 12478: return "%1's casting is interrupted!";                             // INTERRUPT_SPELL_OTHER
		case 12686: return "Your song ends abruptly.";                                // SONG_ENDS_ABRUPTLY
		case 12687: return "Your song ends.";                                         // SONG_ENDS
		case 12688: return "%1's song ends.";                                         // SONG_ENDS_OTHER
		// Additional spell feedback — resist-immunity messages, insufficient mana,
		// spell-needs-target, and other common caster feedback.
		case 241:   return "Your target is immune to the stun portion of this effect."; // IMMUNE_STUN
		case 199:   return "Insufficient Mana to cast this spell!";                   // INSUFFICIENT_MANA
		case 214:   return "You must first select a target for this spell!";           // SPELL_NEED_TAR
		case 191:   return "Your target has no mana to affect.";                      // TARGET_NO_MANA
		case 108:   return "You cannot see your target.";                             // CANT_SEE_TARGET
		case 5817:  return "Your target avoided your %1 ability.";                    // PHYSICAL_RESIST_FAIL
		// Channeling / movement during casting feedback
		case 270:   return "You regain your concentration and continue your casting."; // REGAIN_AND_CONTINUE
		case 1033:  return "%1 regains concentration and continues casting.";          // OTHER_REGAIN_CAST
		// Missing reagent / spell component feedback
		case 272:   return "You are missing some required spell components.";          // MISSING_SPELL_COMP
		case 433:   return "You are missing %1.";                                     // MISSING_SPELL_COMP_ITEM
		// Tradeskill combine feedback — Object::HandleCombine + TradeskillExecute
		// fire these via MessageString.  Without these the v29c client sees nothing
		// when they press Combine: no "no recipe" line, no fail/success message,
		// no learn-recipe nag.  All routed through HandleOutgoingFormattedMessage.
		case 334:   return "You cannot combine these items in this container type!"; // TRADESKILL_NOCOMBINE
		case 336:   return "You lacked the skills to fashion the items together.";  // TRADESKILL_FAILED
		case 338:   return "You can no longer advance your skill from making this item."; // TRADESKILL_TRIVIAL
		case 339:   return "You have fashioned the items together to create something new!"; // TRADESKILL_SUCCEED
		case 3455:  return "You are missing a %1.";                                  // TRADESKILL_MISSING_ITEM
		case 3456:  return "Sorry, but you don't have everything you need for this recipe in your general inventory."; // TRADESKILL_MISSING_COMPONENTS
		case 3457:  return "You have learned the recipe %1!";                        // TRADESKILL_LEARN_RECIPE
		case 6199:  return "Combine would result in a LORE item (%1) you already possess."; // TRADESKILL_COMBINE_LORE
		default:    return nullptr;
	}
}

void TrilogyClient::HandleOutgoingSpecialMesg(const EQApplicationPacket* app)
{
	if (!app || app->size < 25) return;

	const uint8_t* buf  = app->pBuffer;
	uint32_t       size = app->size;

	// Extract EQEmu Chat::* type at offset 3 (after 3 int8 header bytes).
	uint32_t eqemu_type = *reinterpret_cast<const uint32_t*>(buf + 3);

	// Skip speak_mode(1) + journal_mode(1) + language(1) + type(4) + target_id(4) = 11 bytes.
	uint32_t o = 11;

	// Skip null-terminated sender string.
	while (o < size && buf[o] != 0) ++o;
	++o; // consume null
	if (o + 12 >= size) return; // need x(4)+y(4)+z(4) + at least 1 message byte

	// Skip x(4) + y(4) + z(4).
	o += 12;
	if (o >= size) return;

	std::string msg_str = StripSayLinks(reinterpret_cast<const char*>(buf + o), size - o);
	if (msg_str.empty() || msg_str.back() != '\0') msg_str += '\0';

	// Build EQClassic OP_SpecialMesg (0x8021): int32 msg_type + message[].
	uint32_t out_size = 4 + static_cast<uint32_t>(msg_str.size());
	auto* out = new uint8_t[out_size]();
	*reinterpret_cast<uint32_t*>(out) = ChatTypeToTrilogyMsgType(eqemu_type);
	memcpy(out + 4, msg_str.data(), msg_str.size());

	QueueTextPacket(0x8021, out, out_size);
	delete[] out;
}

// ============================================================
// HandleOutgoingFormattedMessage — OP_FormattedMessage (server → client)
//
// EQEmu uses OP_FormattedMessage for NPC speech (say/shout) and many
// system messages.  The packet carries a string_id (resolved client-side
// from eqstr_us.txt) plus null-terminated parameter strings.
//
// Trilogy has no OP_FormattedMessage equivalent.  For the NPC speech /
// emote string IDs we know the parameter layout (param0=speaker, param1=text)
// and re-encode them as OP_SpecialMesg (0x8021) with a pre-formatted line and
// the matching MESSAGETYPE_* color so the client renders them properly.
//   GENERIC_SAY   (1032, "%1 says '%2'")   → MESSAGETYPE_Say   (256)
//   GENERIC_SHOUT (1034, "%1 shouts '%2'") → MESSAGETYPE_Shout (262)
//   GENERIC_EMOTE (1036, "%1 %2")          → MESSAGETYPE_Say   (256)  (color match)
// All other string IDs are silently dropped.
// ============================================================

void TrilogyClient::HandleOutgoingFormattedMessage(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(FormattedMessage_Struct)) return;

	const auto* fm = reinterpret_cast<const FormattedMessage_Struct*>(app->pBuffer);

	const char* base      = fm->message;
	uint32_t    remaining = app->size - static_cast<uint32_t>(sizeof(FormattedMessage_Struct));
	if (remaining < 2) return;

	// param0 = first null-terminated parameter string
	const char* param0 = base;
	size_t      p0len  = strnlen(param0, remaining);

	// Loot messages: string_id 467 ("--You have looted a %1--"), param0 = item link.
	// string_id 466 ("-- %1 has looted a %2--"), param0 = player name, param1 = item link.
	if (fm->string_id == LOOTED_MESSAGE) {
		std::string item_name = StripSayLinks(param0, static_cast<uint32_t>(p0len));
		while (!item_name.empty() && item_name.back() == '\0') item_name.pop_back();
		std::string msg = fmt::format("--You have looted a {}--", item_name);
		msg += '\0';
		uint32_t out_size = 4 + static_cast<uint32_t>(msg.size());
		auto* out = new uint8_t[out_size]();
		*reinterpret_cast<uint32_t*>(out) = 4u; // DARK_BLUE — Trilogy MessageFormat color
		memcpy(out + 4, msg.data(), msg.size());
		QueueTextPacket(0x8021, out, out_size);
		delete[] out;
		return;
	}

	if (fm->string_id == OTHER_LOOTED_MESSAGE && p0len < remaining) {
		const char* param1  = base + p0len + 1;
		uint32_t    p1rem   = remaining - static_cast<uint32_t>(p0len + 1);
		std::string item_name = StripSayLinks(param1, p1rem);
		while (!item_name.empty() && item_name.back() == '\0') item_name.pop_back();
		std::string player_name(param0, p0len);
		std::string msg = fmt::format("--{} has looted a {}--", player_name, item_name);
		msg += '\0';
		uint32_t out_size = 4 + static_cast<uint32_t>(msg.size());
		auto* out = new uint8_t[out_size]();
		*reinterpret_cast<uint32_t*>(out) = 4u; // DARK_BLUE — Trilogy MessageFormat color
		memcpy(out + 4, msg.data(), msg.size());
		QueueTextPacket(0x8021, out, out_size);
		delete[] out;
		return;
	}

	// Faction / experience / level system messages: resolve the template and relay
	// as OP_SpecialMesg (verbatim) — the same path that works for the #dev menu /
	// combat records.  %1..%9 are filled from the null-separated message args.
	const char* tmpl = TrilogySystemStringTemplate(fm->string_id);
	if (tmpl) {
		// Resist string IDs: flush deferred OP_CastOn with "not landed" so the
		// v29c client plays the spell animation without adding a buff icon.
		// 425 = TARGET_RESISTED, 426 = YOU_RESIST, 5817 = PHYSICAL_RESIST_FAIL
		if (fm->string_id == 425 || fm->string_id == 426 || fm->string_id == 5817) {
			FlushPendingCastOn(false);

			// Self-cast: suppress the redundant caster-perspective message.
			// The player already sees YOU_RESIST (426); TARGET_RESISTED (425)
			// is the first message that arrives and makes it look like a duplicate.
			if (fm->string_id == 425 && m_last_caston_was_self_resist)
				return;
		}

		// Gather up to 9 null-separated args.
		std::vector<std::string> args;
		const char* p   = base;
		uint32_t    rem = remaining;
		while (rem > 0 && args.size() < 9) {
			size_t l = strnlen(p, rem);
			args.emplace_back(p, l);
			if (l >= rem) break;
			p   += l + 1;
			rem -= static_cast<uint32_t>(l + 1);
		}

		std::string out_text = StripSayLinks(tmpl, strlen(tmpl));
		for (size_t i = 0; i < args.size(); ++i) {
			const std::string token = "%" + std::to_string(i + 1);
			for (size_t pos; (pos = out_text.find(token)) != std::string::npos; )
				out_text.replace(pos, token.size(), args[i]);
		}
		out_text += '\0';

		// Honor the modern Chat::* channel the server picked (fm->type) instead
		// of hardcoding white.  ChatTypeToTrilogyMsgType handles the special
		// modern-only types we care about (Chat::Experience → YELLOW,
		// Chat::SpellFailure → RED), passes raw colors 0-20 through, and falls
		// back to broadcasts for the rest.
		uint32_t out_color = ChatTypeToTrilogyMsgType(fm->type);

		uint32_t out_size = 4 + static_cast<uint32_t>(out_text.size());
		auto* out = new uint8_t[out_size]();
		*reinterpret_cast<uint32_t*>(out) = out_color;
		memcpy(out + 4, out_text.data(), out_text.size());
		QueueTextPacket(0x8021, out, out_size);
		delete[] out;
		return;
	}

	// GENERIC_EMOTE (1036, "%1 %2") — Mob::Emote / quest::emote / Lua mob:emote.
	// param0 = sender name, param1 = emote text (already verb-phrase, e.g.
	// "bows deeply.").  Render server-side as "<name> <text>" on MESSAGETYPE_Emote
	// (263) — same SpecialMesg path that GENERIC_SAY/SHOUT use, since v29c has
	// no FormattedMessage equivalent and chan-8 ChannelMessage truncates / mangles
	// brackets.
	if (fm->string_id == GENERIC_EMOTE) {
		if (p0len >= remaining) return;
		const char* param1      = base + p0len + 1;
		uint32_t    p1remaining = remaining - static_cast<uint32_t>(p0len + 1);
		if (p1remaining < 1) return;

		std::string text = StripSayLinks(param1, p1remaining);
		while (!text.empty() && text.back() == '\0') text.pop_back();

		std::string sender(param0, p0len);
		std::string line = fmt::format("{} {}", sender, text);
		line += '\0';

		uint32_t out_size = 4 + static_cast<uint32_t>(line.size());
		auto* out = new uint8_t[out_size]();
		// Use MESSAGETYPE_Say (256) — same color as NPC say.  MESSAGETYPE_Emote
		// (263) renders as the channel-emote blue, which doesn't match the
		// EQEmu Mob::Emote intent (Chat::NPCQuestSay color, used for in-world
		// scene flavor like NPC bows / kneels).
		*reinterpret_cast<uint32_t*>(out) = 256u; // MESSAGETYPE_Say
		memcpy(out + 4, line.data(), line.size());
		QueueTextPacket(0x8021, out, out_size);
		delete[] out;
		return;
	}

	bool is_shout;
	if (fm->string_id == GENERIC_SHOUT)
		is_shout = true;
	else if (fm->string_id == GENERIC_SAY)
		is_shout = false;
	else
		return;

	// param1 = message text (second null-terminated string after param0)
	if (p0len >= remaining) return;
	const char* param1      = base + p0len + 1;
	uint32_t    p1remaining = remaining - static_cast<uint32_t>(p0len + 1);
	if (p1remaining < 1) return;

	std::string text = StripSayLinks(param1, p1remaining);
	while (!text.empty() && text.back() == '\0') text.pop_back();

	// Deliver NPC speech as a pre-formatted OP_SpecialMesg (0x8021) string rather
	// than a chan-8 ChannelMessage (0x0721).  The PC Trilogy client's chan-8 SAY
	// renderer truncates long lines and mangles [keyword] brackets (showing only the
	// trailing ']'), but the SpecialMesg path — used for the #dev menu, combat
	// records, etc. — renders long bracketed text verbatim.  EQClassic likewise
	// formats NPC dialogue server-side ("<name> says, '<text>'") instead of letting
	// the client wrap a chan-8 message.
	std::string sender(param0, p0len);
	std::string line = fmt::format("{} {}, '{}'", sender, is_shout ? "shouts" : "says", text);
	line += '\0';

	const uint32_t msg_type = is_shout ? 262u   // MESSAGETYPE_Shout
	                                   : 256u;  // MESSAGETYPE_Say

	uint32_t out_size = 4 + static_cast<uint32_t>(line.size());
	auto* out = new uint8_t[out_size]();
	*reinterpret_cast<uint32_t*>(out) = msg_type;
	memcpy(out + 4, line.data(), line.size());
	QueueTextPacket(0x8021, out, out_size);
	delete[] out;
}

// ============================================================
// HandleOutgoingSimpleMessage — OP_SimpleMessage (server → client)
//
// EQEmu sends no-argument system messages (e.g. GAIN_XP "You gain experience!!")
// as OP_SimpleMessage: a bare string_id + color, no text.  The Trilogy client has
// no equivalent, so resolve the known string-ids to text and relay as OP_SpecialMesg.
// ============================================================
void TrilogyClient::HandleOutgoingSimpleMessage(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(SimpleMessage_Struct)) return;

	const auto* sm   = reinterpret_cast<const SimpleMessage_Struct*>(app->pBuffer);
	const char* tmpl = TrilogySystemStringTemplate(sm->string_id);
	if (!tmpl) return; // not a string-id we relay

	std::string text = tmpl;
	text += '\0';

	uint32_t out_color = ChatTypeToTrilogyMsgType(sm->color);

	uint32_t out_size = 4 + static_cast<uint32_t>(text.size());
	auto* out = new uint8_t[out_size]();
	*reinterpret_cast<uint32_t*>(out) = out_color;
	memcpy(out + 4, text.data(), text.size());
	QueueTextPacket(0x8021, out, out_size);
	delete[] out;
}

// ============================================================
// SendTrilogyMoneyDelta — refresh the client's money display by the given coin
// delta via OP_TradeMoneyUpdate (0x3d21), one packet per non-zero denomination.
//
// EQClassic's SendClientMoneyUpdate(type, amount) is INCREMENTAL — the client adds
// `amount` of denomination `type` (0=cp 1=sp 2=gp 3=pp) to its display.  Coin
// rewards (givecash / QuestReward) are deltas the server already added to the
// PlayerProfile, so relaying them here keeps the live display in sync without a relog.
// ============================================================
void TrilogyClient::SendTrilogyMoneyDelta(uint32 copper, uint32 silver, uint32 gold, uint32 platinum)
{
	struct TradeMoneyUpdate { int32_t trader; int32_t type; int32_t amount; };
	auto push = [&](int32_t type, int32_t amount) {
		if (amount <= 0) return;
		TradeMoneyUpdate u{ 0, type, amount };
		m_tzs->SendToSession(m_session_key, 0x3d21,
		                     reinterpret_cast<const uint8_t*>(&u), sizeof(u));
	};
	push(0, static_cast<int32_t>(copper));
	push(1, static_cast<int32_t>(silver));
	push(2, static_cast<int32_t>(gold));
	push(3, static_cast<int32_t>(platinum));
}

// ============================================================
// HandleBeginCast — OP_BeginCast (server → Trilogy client).
//
// Shows the casting animation and cast bar for any entity in range.
// EQEmu sends this to all nearby clients when anyone begins casting.
//
// EQEmu BeginCast_Struct: { uint16 caster_id, uint16 spell_id, uint32 cast_time }
// Trilogy BeginCast_Struct: { int32 caster_id, int32 spell_id, int32 cast_time }
// ============================================================

void TrilogyClient::HandleBeginCast(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::BeginCast_Struct)) return;
	const auto* emu = reinterpret_cast<const ::BeginCast_Struct*>(app->pBuffer);

	Trilogy::structs::BeginCast_Struct out{};
	out.caster_id = static_cast<int32_t>(TranslateId(emu->caster_id));
	out.spell_id  = static_cast<int32_t>(emu->spell_id);
	out.cast_time = static_cast<int32_t>(emu->cast_time);

	// Diag: log caster NPC race/class/level/model + name when possible — when
	// v29c freezes during a hostile cast, knowing the caster's properties tells
	// us whether it's a PC-race NPC, a specific texture, an unusual model, etc.
	Mob* caster = entity_list.GetMob(static_cast<uint16>(emu->caster_id));
	if (caster) {
		LogInfo("[TrilogyDiag] A920 BeginCast caster_id={} spell_id={} cast_time={} name='{}' race={} class={} level={} gender={} texture={} helmtex={} size={} npc={}",
		        out.caster_id, out.spell_id, out.cast_time,
		        caster->GetCleanName(),
		        static_cast<int>(caster->GetRace()),
		        static_cast<int>(caster->GetClass()),
		        static_cast<int>(caster->GetLevel()),
		        static_cast<int>(caster->GetGender()),
		        static_cast<int>(caster->GetTexture()),
		        static_cast<int>(caster->GetHelmTexture()),
		        static_cast<double>(caster->GetSize()),
		        caster->IsNPC() ? 1 : 0);
	} else {
		LogInfo("[TrilogyDiag] A920 BeginCast caster_id={} spell_id={} cast_time={} (mob not in entity_list)",
		        out.caster_id, out.spell_id, out.cast_time);
	}

	m_tzs->SendToSession(m_session_key, 0xa920,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleAction — OP_Action (server → Trilogy client).
//
// For spells (type == 231 / 0xE7):
//   DEFERS OP_CastOn (0x4620) — see FlushPendingCastOn.  EQEmu sends
//   OP_Action before the resist check, but the v29c client's unknown2[1]
//   byte (0x04 = add buff icon, 0x00 = animation only) must vary by
//   outcome, so we store the packet and flush it when the outcome is known.
// For melee types (type != 231):
//   Sends OP_Action (0x5820) immediately.
// ============================================================

// ============================================================
// FindParentSpellForRecourse — reverse-recourse lookup for v29c.
//
// Modern EQEmu splits some classic-era spells into primary + recourse:
// Siphon Strength (343) applies -5 STR on target, then fires spell
// 2463 ("Siphon Strength Recourse") on the caster for the +5 STR
// self-buff.  Classic v29c's local spells file predates this split —
// the client only knows spell 343, whose classic data carried BOTH
// effects in its slots.  Sending recourse spell_ids on the wire makes
// the client fall through to the item icon table (user reported seeing
// "Cloth Choker" on the buff bar), and the client can't compute buff
// stat bonuses because the "spell" doesn't exist in its file.
//
// Used by HandleAction (CastOn 0x4620 with unknown2[1]=0x04 — the
// packet that ADDS the icon) and HandleBuff (OP_Buff 0x3221 — duration
// updates for existing buffs) to translate self-target recourse
// spell_ids back to the parent's ID so the classic client sees the
// spell it knows.
// ============================================================

static uint16 FindParentSpellForRecourse(uint16 recourse_spell_id)
{
	if (recourse_spell_id == 0 || recourse_spell_id >= SPDAT_RECORDS)
		return 0;
	for (int i = 1; i < SPDAT_RECORDS; ++i) {
		if (i == recourse_spell_id) continue;
		if (spells[i].recourse_link == recourse_spell_id)
			return static_cast<uint16>(i);
	}
	return 0;
}

// ============================================================
// FlushPendingCastOn — send a deferred OP_CastOn (0x4620).
//
// EQEmu fires OP_Action BEFORE the resist check; EQClassic fires
// OP_CastOn AFTER, with unknown2[1]=0x04 when the spell lands
// and 0x00 when it is resisted.  0x04 makes the v29c client add
// the buff/debuff icon; without it the animation plays but no
// icon is placed.  We defer OP_CastOn in HandleAction and flush
// here once the outcome is known.
// ============================================================

void TrilogyClient::FlushPendingCastOn(bool spell_landed)
{
	if (!m_pending_caston_active) return;
	m_pending_caston_data.unknown2[1] =
		static_cast<int8_t>(spell_landed ? 0x04 : 0x00);

	LogInfo("[TrilogyDiag] 4620 CastOn source_id={} target_id={} spell_id={} landed={}",
	        m_pending_caston_data.source_id, m_pending_caston_data.target_id,
	        m_pending_caston_data.spell_id, spell_landed ? 1 : 0);

	m_tzs->SendToSession(m_session_key, 0x4620,
	                     reinterpret_cast<const uint8_t*>(&m_pending_caston_data),
	                     static_cast<uint32_t>(sizeof(m_pending_caston_data)));
	// Track whether this was a self-cast resist for duplicate-message suppression.
	m_last_caston_was_self_resist = !spell_landed &&
		m_pending_caston_data.target_id == m_pending_caston_data.source_id &&
		m_pending_caston_data.target_id != 0;
	m_pending_caston_active = false;
	memset(&m_pending_caston_data, 0, sizeof(m_pending_caston_data));
}

void TrilogyClient::HandleAction(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Action_Struct)) return;
	const auto* emu = reinterpret_cast<const ::Action_Struct*>(app->pBuffer);

	// Spells (type 231) use OP_CastOn (0x4620) — that packet carries
	// both the target animation and the buff-icon signal in Trilogy.
	// OP_Action (0x5820) is only for melee/non-spell combat.
	//
	// EQEmu fires OP_Action BEFORE the resist check, but the v29c client
	// needs different unknown2[1] values depending on the outcome (0x04
	// for landed, 0x00 for resisted).  We defer the send until the
	// outcome is known (see FlushPendingCastOn).
	//
	// Silently overwrite any prior pending — do NOT emit landed=false here.
	// spells.cpp sends TWO OP_Actions per successful cast (initial +
	// effect_flag=0x04 resend at line 4667), and recourse spells inject a
	// third pair before the primary's resend.  Flushing prior pending as
	// landed=false on every new stash produced spurious 4620 packets with
	// unknown2[1]=0x00, which the v29c client interpreted as "resisted —
	// no icon added on target" even when the spell truly landed.  The
	// definitive outcome is emitted by the flush points that DO know it:
	// HandleBuff / HandleDamage / HandleManaChange (landed=true) or the
	// resist string_id handler (landed=false).  A dropped pending whose
	// outcome never arrives (e.g., a check failed after action_packet was
	// queued) correctly results in "animation only, no icon" on the wire.
	if (emu->type == 231) {
		m_pending_caston_active = false;
		memset(&m_pending_caston_data, 0, sizeof(m_pending_caston_data));
		m_last_caston_was_self_resist = false;

		// Recourse translation for self-cast buff icons.  See
		// FindParentSpellForRecourse.  The CastOn packet with unknown2[1]=0x04
		// is what tells the v29c client to ADD the icon to the buff bar; if
		// spell_id is a recourse spell the client doesn't know (it predates
		// modern EQEmu's primary/recourse split), the icon-add silently fails
		// and buff stat bonuses can't be computed.  For self-cast recourse
		// spells (source == target), send the parent spell_id so the client
		// sees the classic spell (e.g. Siphon Strength 343 instead of Siphon
		// Strength Recourse 2463).
		uint16 wire_spell_id = static_cast<uint16>(emu->spell);
		if (emu->source == emu->target) {
			uint16 parent = FindParentSpellForRecourse(wire_spell_id);
			if (parent != 0)
				wire_spell_id = parent;
		}

		Trilogy::structs::CastOn_Struct caston{};
		memset(&caston, 0, sizeof(caston));
		caston.target_id        = static_cast<int32_t>(TranslateId(emu->target));
		caston.source_id        = static_cast<int32_t>(TranslateId(emu->source));
		caston.source_level     = static_cast<int8_t>(emu->level);
		caston.unknown1[1]      = static_cast<int8_t>(0x41);
		caston.heading          = emu->hit_heading * 2.0f;
		caston.unknown_zero2[0] = static_cast<int8_t>(0x0A);
		caston.action           = 231;
		caston.spell_id         = static_cast<int16_t>(wire_spell_id);

		m_pending_caston_data   = caston;
		m_pending_caston_active = true;
		return;
	}

	// Combat-event visibility filter — drop melee actions where neither
	// source nor target is close enough to be visually relevant.
	if (!IsCombatEventCloseToObserver(emu->source, emu->target))
		return;

	Trilogy::structs::Action_Struct out{};
	memset(&out, 0, sizeof(out));
	out.target = static_cast<int32_t>(TranslateId(emu->target));
	out.source = static_cast<int32_t>(TranslateId(emu->source));
	out.type   = static_cast<int8_t>(emu->type);
	out.spell  = static_cast<int16_t>(emu->spell);

	// Routed through combat-event token bucket — see QueueTextPacket comment.
	QueueTextPacket(0x5820, reinterpret_cast<const uint8_t*>(&out),
	                static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleDamage — OP_Damage (server → Trilogy client).
//
// Sends the melee/spell damage number. Trilogy uses the same opcode
// (0x5820) and Action_Struct for both OP_Action and OP_Damage; the
// presence of a non-zero damage field distinguishes them client-side.
//
// EQEmu CombatDamage_Struct: { uint16 target, uint16 source, uint8 type,
//   uint16 spellid, uint32 damage, float force, float hit_heading,
//   float hit_pitch, uint32 special }
// ============================================================

void TrilogyClient::HandleDamage(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::CombatDamage_Struct)) return;
	const auto* emu = reinterpret_cast<const ::CombatDamage_Struct*>(app->pBuffer);

	// Spell damage (DD spells): the spell landed — flush pending OP_CastOn.
	// DD spells don't generate OP_Buff, so this is the only flush point.
	// NOTE: FlushPendingCastOn runs even when the event is too far to
	// broadcast — the CastOn buff state must be cleared regardless of
	// whether the damage packet itself is going on the wire.
	if (emu->spellid != 0 && emu->spellid != 0xFFFF)
		FlushPendingCastOn(true);

	// Combat-event visibility filter — drop damage events where neither
	// source nor target is close enough to be visually relevant.
	if (!IsCombatEventCloseToObserver(emu->source, emu->target))
		return;

	Trilogy::structs::Action_Struct out{};
	memset(&out, 0, sizeof(out));
	out.target = static_cast<int32_t>(TranslateId(emu->target));
	out.source = static_cast<int32_t>(TranslateId(emu->source));
	out.type   = static_cast<int8_t>(emu->type);
	out.spell  = static_cast<int16_t>(emu->spellid);
	// Reinterpret as signed int32.  EQEmu stores miss/block/parry/dodge/riposte/rune
	// as negative sentinels (DMG_BLOCKED=-1, DMG_PARRIED=-2, … DMG_RUNE=-6) in an
	// int64 that then gets packed into CombatDamage_Struct::damage as uint32.
	// Those values arrive here as 0xFFFFFFFF, 0xFFFFFFFE … — all > INT32_MAX.
	// The old clamp to INT32_MAX turned every miss/block/etc. into a displayed hit
	// of 2,147,483,647 pts that appeared to one-shot the player.
	// Action_Struct::damage is signed (see trilogy_structs.h), so negative values
	// are exactly what the client expects for non-hit outcomes.
	out.damage = static_cast<int32_t>(emu->damage);

	// Routed through combat-event token bucket — see QueueTextPacket comment.
	QueueTextPacket(0x5820, reinterpret_cast<const uint8_t*>(&out),
	                static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleManaChange — OP_ManaChange (server → Trilogy client).
//
// Sent after a spell is cast to update the caster's displayed mana bar.
// Trilogy ManaChange_Struct is 4 bytes: { uint16 new_mana, uint16 spell_id }.
//
// When spell_id > 0 and keepcasting == 0 this is the "spell complete,
// enable spellbar" signal (SendSpellBarEnable).  The v29c client may
// un-grey all gems in response, so we immediately re-grey any gems
// whose server-tracked cooldowns are still active.
// ============================================================

void TrilogyClient::HandleManaChange(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::ManaChange_Struct)) return;
	const auto* emu = reinterpret_cast<const ::ManaChange_Struct*>(app->pBuffer);

	// Safety: if a pending OP_CastOn still exists by the time the "spell
	// complete" mana update arrives, flush it now.  This covers edge cases
	// like utility spells that produce neither OP_Buff nor OP_Damage.
	if (emu->keepcasting == 0 && emu->spell_id > 0)
		FlushPendingCastOn(true);

	Trilogy::structs::ManaChange_Struct out{};
	out.new_mana = static_cast<uint16_t>(
	    emu->new_mana > 0xFFFFu ? 0xFFFFu : emu->new_mana);
	// Trilogy wire struct has no keepcasting field.  A non-zero spell_id
	// tells the v29c client "cast complete, re-enable spellbar".  Mid-cast
	// mana updates (keepcasting=1, from regen ticks) must send spell_id=0
	// so the client doesn't dismiss the cast bar prematurely.
	out.spell_id = (emu->keepcasting == 0)
	    ? static_cast<uint16_t>(emu->spell_id) : 0;

	m_tzs->SendToSession(m_session_key, 0x7f21,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));

	// Re-grey gems still on cooldown.  SendSpellBarEnable (keepcasting=0,
	// spell_id>0) may have un-greyed them; push them back to grey state.
	if (emu->keepcasting == 0 && emu->spell_id != 0) {
		uint64_t now_ms = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());

		for (uint32_t i = 0; i < Trilogy::structs::SPELL_MEMORY_SIZE; ++i) {
			if (!m_gem_cooldowns[i].active) continue;
			if (now_ms >= m_gem_cooldowns[i].end_ms) {
				m_gem_cooldowns[i].active = false;
				continue;
			}

			Trilogy::structs::MemorizeSpell_Struct mem{};
			mem.slot     = static_cast<int32_t>(i);
			mem.spell_id = static_cast<int32_t>(m_gem_cooldowns[i].spell_id);
			mem.scribing = 3;  // grey-out

			m_tzs->SendToSession(m_session_key, 0x8221,
			                     reinterpret_cast<const uint8_t*>(&mem),
			                     static_cast<uint32_t>(sizeof(mem)));
		}
	}
}

// ============================================================
// HandleHPUpdate — OP_HPUpdate (server → Trilogy client, self-only).
//
// Sent by the server to the player themselves to update their own HP display.
//
// EQEmu SpawnHPUpdate_Struct: { uint32 cur_hp, int32 max_hp, int16 spawn_id }
// Trilogy SpawnHPUpdate_Struct: { int32 spawn_id, int32 cur_hp, int32 max_hp }
// ============================================================

void TrilogyClient::HandleHPUpdate(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::SpawnHPUpdate_Struct)) return;
	const auto* emu = reinterpret_cast<const ::SpawnHPUpdate_Struct*>(app->pBuffer);

	Trilogy::structs::SpawnHPUpdate_Struct out{};
	out.spawn_id = static_cast<int32_t>(TranslateId(static_cast<uint32_t>(static_cast<uint16_t>(emu->spawn_id))));
	out.cur_hp   = static_cast<int32_t>(emu->cur_hp);
	out.max_hp   = emu->max_hp;

	// B220 unreliable: see HandleHPUpdate notes / Agz EQPacketManager.cpp:412.
	// Next HP change supersedes; loss is harmless.
	m_tzs->SendToSession(m_session_key, 0xb220,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)),
	                     /*ack_req=*/false);
}

// ============================================================
// HandleMobHealth — OP_MobHealth (server → Trilogy client).
//
// Sent by QueueClientsByTarget when a targeted mob's HP changes.
// EQEmu sends a percentage-only struct (SpawnHPUpdate_Struct2); Trilogy
// needs absolute HP values, so look up the mob in the entity list.
//
// EQEmu SpawnHPUpdate_Struct2: { int16 spawn_id, uint8 hp_percent }
// Trilogy SpawnHPUpdate_Struct: { int32 spawn_id, int32 cur_hp, int32 max_hp }
// ============================================================

void TrilogyClient::HandleMobHealth(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::SpawnHPUpdate_Struct2)) return;
	const auto* emu = reinterpret_cast<const ::SpawnHPUpdate_Struct2*>(app->pBuffer);

	Mob* mob = entity_list.GetMob(static_cast<uint16>(emu->spawn_id));
	if (!mob) return;

	Trilogy::structs::SpawnHPUpdate_Struct out{};
	out.spawn_id = static_cast<int32_t>(TranslateId(static_cast<uint32_t>(static_cast<uint16_t>(emu->spawn_id))));
	out.cur_hp   = static_cast<int32_t>(mob->GetHP());
	out.max_hp   = static_cast<int32_t>(mob->GetMaxHP());

	// B220 unreliable: see HandleHPUpdate notes / Agz EQPacketManager.cpp:412.
	m_tzs->SendToSession(m_session_key, 0xb220,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)),
	                     /*ack_req=*/false);
}

// ============================================================
// HandleMemorizeSpellOut — OP_MemorizeSpell (server → Trilogy client).
//
// Server sends this to confirm spell memorization, book scribing, or
// gem un-memorization after processing a client request.
//
// EQEmu scribing: 0=scribe, 1=memorize, 2=forget, 3=memSpellSpellbar.
// Trilogy scribing: 0=scribe, 1=memorize, 3=gray-out gem (forget).
//
// memSpellSpellbar (3) is sent by CastedSpellFinished to update gem
// cooldown state after a spell completes.  For Trilogy we translate
// this into an explicit grey-out (scribing=3) for the cast gem, and
// track the cooldown so CheckSpellGemCooldowns can un-grey it later.
// ============================================================

void TrilogyClient::HandleMemorizeSpellOut(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::MemorizeSpell_Struct)) return;
	const auto* emu = reinterpret_cast<const ::MemorizeSpell_Struct*>(app->pBuffer);

	// Drop slot >= 8: catches Ability(20), Item(22), Discipline(23)
	// internal slot values that would overflow v29c pp.spell_memory[8].
	if (emu->slot >= Trilogy::structs::SPELL_MEMORY_SIZE) return;

	if (emu->scribing == memSpellSpellbar) {
		// SPELLBAR_UNLOCK (spell_id 0x2bc) is the global "re-enable spellbar"
		// signal from SendSpellBarDisable; v29c handles this via OP_ManaChange
		// so we drop it here.
		if (emu->spell_id == SPELLBAR_UNLOCK) return;
		if (!IsValidSpell(emu->spell_id)) return;

		uint32_t recast_ms   = spells[emu->spell_id].recast_time;
		uint32_t reduction   = emu->reduction;
		uint32_t cooldown_ms = (recast_ms > reduction) ? (recast_ms - reduction) : 0;

		// Only grey spells with a meaningful per-spell recast beyond the GCD.
		// The server enforces the recast check at >1000 ms (spells.cpp:1465).
		if (cooldown_ms <= 1500) return;

		uint64_t now_ms = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());

		m_gem_cooldowns[emu->slot].spell_id = emu->spell_id;
		m_gem_cooldowns[emu->slot].end_ms   = now_ms + cooldown_ms;
		m_gem_cooldowns[emu->slot].active   = true;

		Trilogy::structs::MemorizeSpell_Struct out{};
		out.slot     = static_cast<int32_t>(emu->slot);
		out.spell_id = static_cast<int32_t>(emu->spell_id);
		out.scribing = 3;  // grey-out gem

		m_tzs->SendToSession(m_session_key, 0x8221,
		                     reinterpret_cast<const uint8_t*>(&out),
		                     static_cast<uint32_t>(sizeof(out)));
		return;
	}

	// Memorize (1) or forget (2→3): clear any active cooldown on this slot.
	if (emu->scribing == memSpellMemorize || emu->scribing == memSpellForget) {
		m_gem_cooldowns[emu->slot].active = false;
	}

	Trilogy::structs::MemorizeSpell_Struct out{};
	out.slot     = static_cast<int32_t>(emu->slot);
	out.spell_id = static_cast<int32_t>(emu->spell_id);
	out.scribing = static_cast<int32_t>(emu->scribing);

	m_tzs->SendToSession(m_session_key, 0x8221,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// CheckSpellGemCooldowns — called from TrilogyZoneServer::Tick().
//
// Un-greys spell gems whose recast cooldowns have expired.
// Uses OP_ManaChange (the EQClassic EnableSpellBar mechanism)
// to signal "spellbar ready" — this un-greys gems without
// triggering the "Finished memorizing" message that scribing=1
// would produce.  After the ManaChange, any gems whose cooldowns
// are still running are immediately re-greyed.
// ============================================================

void TrilogyClient::CheckSpellGemCooldowns()
{
	uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	bool any_expired = false;
	for (uint32_t i = 0; i < Trilogy::structs::SPELL_MEMORY_SIZE; ++i) {
		if (!m_gem_cooldowns[i].active) continue;
		if (now_ms >= m_gem_cooldowns[i].end_ms) {
			m_gem_cooldowns[i].active = false;
			any_expired = true;
		}
	}

	if (!any_expired) return;

	// OP_ManaChange with spell_id > 0 tells the client the spellbar is
	// enabled (same as EQClassic EnableSpellBar).  This un-greys gems.
	Trilogy::structs::ManaChange_Struct mc{};
	uint32_t mana = static_cast<uint32_t>(GetMana());
	mc.new_mana = static_cast<uint16_t>(mana > 0xFFFFu ? 0xFFFFu : mana);
	mc.spell_id = 1;

	m_tzs->SendToSession(m_session_key, 0x7f21,
	                     reinterpret_cast<const uint8_t*>(&mc),
	                     static_cast<uint32_t>(sizeof(mc)));

	// Re-grey gems whose cooldowns are still running.
	for (uint32_t i = 0; i < Trilogy::structs::SPELL_MEMORY_SIZE; ++i) {
		if (!m_gem_cooldowns[i].active) continue;

		Trilogy::structs::MemorizeSpell_Struct mem{};
		mem.slot     = static_cast<int32_t>(i);
		mem.spell_id = static_cast<int32_t>(m_gem_cooldowns[i].spell_id);
		mem.scribing = 3;

		m_tzs->SendToSession(m_session_key, 0x8221,
		                     reinterpret_cast<const uint8_t*>(&mem),
		                     static_cast<uint32_t>(sizeof(mem)));
	}
}

// ============================================================
// HandleBuff — OP_Buff (server → Trilogy client).
//
// Sent when a buff is applied or removed to update buff icons.
// EQEmu SpellBuffPacket_Struct has a nested SpellBuff_Struct with
// the spell ID; Trilogy Buff_Struct is a flat 20-byte layout.
// ============================================================

void TrilogyClient::HandleBuff(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::SpellBuffPacket_Struct)) return;
	const auto* emu = reinterpret_cast<const ::SpellBuffPacket_Struct*>(app->pBuffer);

	// Buff application (bufffade==0): the spell landed — flush the deferred
	// OP_CastOn with unknown2[1]=0x04 so the v29c client adds the buff icon.
	if (emu->bufffade == 0)
		FlushPendingCastOn(true);

	uint16 wire_spell_id = static_cast<uint16>(emu->buff.spellid);

	// Recourse translation (self-buffs only).  See FindParentSpellForRecourse.
	// Restrict to same-entity packets — a debuff landing on someone else
	// keeps its own spell_id.
	if (emu->entityid == GetID()) {
		uint16 parent = FindParentSpellForRecourse(wire_spell_id);
		if (parent != 0)
			wire_spell_id = parent;
	}

	Trilogy::structs::Buff_Struct out{};
	memset(&out, 0, sizeof(out));
	out.target_id = TranslateId(emu->entityid);
	out.spell_id  = wire_spell_id;
	out.buff_slot = emu->slotid;

	m_tzs->SendToSession(m_session_key, 0x3221,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleDeath — OP_Death (server → Trilogy client).
//
// Sent when any entity in the zone dies. Causes the Trilogy client to
// play the death animation and remove the entity from the world.
//
// EQEmu Death_Struct: { uint32 spawn_id, killer_id, corpseid, bindzoneid,
//   spell_id, attack_skill, damage, unknown028 }
// Trilogy Death_Struct: { int32 spawn_id, killer_id, corpseid,
//   int16 unknown12, int8 attack_skill, int8 unknown15,
//   int16 damage, int16 unknown18 }
// ============================================================

void TrilogyClient::HandleDeath(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Death_Struct)) return;
	const auto* emu = reinterpret_cast<const ::Death_Struct*>(app->pBuffer);

	Trilogy::structs::Death_Struct out{};
	memset(&out, 0, sizeof(out));
	out.spawn_id     = static_cast<int32_t>(TranslateId(emu->spawn_id));
	out.killer_id    = static_cast<int32_t>(TranslateId(emu->killer_id));
	out.corpseid     = static_cast<int32_t>(TranslateId(emu->corpseid));
	out.attack_skill = static_cast<int8_t>(emu->attack_skill);
	out.damage       = static_cast<int16_t>(
	    emu->damage > static_cast<uint32_t>(INT16_MAX) ? INT16_MAX : emu->damage);

	m_tzs->SendToSession(m_session_key, 0x4a20,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));

	// Death camera: SAT_SendToBind(1) AFTER OP_Death triggers the v29c
	// 3rd-person death camera.  A duplicate of the one in HandleBecomeCorpse
	// (which fires before OP_Death to create the corpse entity while the
	// spawn still exists).  param=0 does NOT trigger the camera — only 1 does.
	if (TranslateId(emu->spawn_id) == static_cast<uint32_t>(m_player_spawn_id)) {
		Trilogy::structs::SpawnAppearance_Struct sa{};
		sa.spawn_id  = static_cast<int16_t>(m_player_spawn_id);
		sa.type      = 0; // SAT_SendToBind
		sa.parameter = 1; // >0 = triggers death camera when sent after OP_Death
		m_tzs->SendToSession(m_session_key, 0xf520,
		                     reinterpret_cast<const uint8_t*>(&sa),
		                     static_cast<uint32_t>(sizeof(sa)));
	}
}

// ============================================================
// HandleBecomeCorpse — OP_BecomeCorpse (server → Trilogy client).
//
// EQEmu sends OP_BecomeCorpse to tell clients that an entity has
// transitioned from a living spawn to a corpse (player death).
// The v29c client does not understand OP_BecomeCorpse — it uses
// SpawnAppearance(type=0/Die, parameter=1) for this purpose
// (EQClassic: SendAppearancePacket(id, SAT_SendToBind, 1, true)).
// ============================================================

void TrilogyClient::HandleBecomeCorpse(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::BecomeCorpse_Struct)) return;
	const auto* bc = reinterpret_cast<const ::BecomeCorpse_Struct*>(app->pBuffer);

	// Save death position before ZonePC (called by GoToDeath moments later)
	// overwrites m_Position with bind coordinates.
	m_death_x       = bc->x;
	m_death_y       = bc->y;
	m_death_z       = bc->z;
	m_death_heading = GetHeading();
	m_has_death_pos = true;

	// Lock UI (SAT_Position_Update=14, SAPP_Lose_Control=102).
	// EQClassic sends this before Death().
	Trilogy::structs::SpawnAppearance_Struct cam{};
	cam.spawn_id  = static_cast<int16_t>(TranslateId(bc->spawn_id));
	cam.type      = 14;  // SAT_Position_Update
	cam.parameter = 102; // SAPP_Lose_Control
	m_tzs->SendToSession(m_session_key, 0xf520,
	                     reinterpret_cast<const uint8_t*>(&cam),
	                     static_cast<uint32_t>(sizeof(cam)));

	// NOTE: do NOT send SAT_SendToBind(1) here.  Sending it before OP_Death
	// puts the entity into corpse state, which prevents the post-death
	// SAT_SendToBind(1) in HandleDeath from triggering the 3rd-person
	// death camera.  The corpse is handled by the post-death send +
	// EQEmu's Corpse entity (visible on zone re-entry via OP_NewSpawn).
}

// ============================================================
// HandleAnimation — OP_Animation (server → Trilogy client).
//
// Sent by Mob::DoAnim to broadcast entity animations (spell casting
// body gesture, melee swings, etc.) to nearby clients.
//
// EQEmu Animation_Struct: { uint16 spawnid, uint8 speed, uint8 action } = 4 bytes
// Trilogy Attack_Struct (OP_Attack 0x9f20): { int32 spawn_id, int8 type, int8[7] unknown } = 12 bytes
//
// EQClassic sets unknown[5]=0x80 and unknown[6]=0x3F in DoAnim.
// ============================================================
void TrilogyClient::HandleAnimation(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Animation_Struct)) return;
	const auto* emu = reinterpret_cast<const ::Animation_Struct*>(app->pBuffer);

	// Combat-event visibility filter — drop swings from sources too far from
	// the observing player to be visually relevant.  Animation has no target
	// field; source-only check.
	if (!IsCombatEventCloseToObserver(emu->spawnid, 0))
		return;

	Trilogy::structs::Attack_Struct out{};
	memset(&out, 0, sizeof(out));
	out.spawn_id         = static_cast<int32_t>(TranslateId(emu->spawnid));
	out.type             = static_cast<int8_t>(emu->action);
	out.a_unknown2[5]    = static_cast<int8_t>(0x80);
	out.a_unknown2[6]    = static_cast<int8_t>(0x3F);

	// Routed through combat-event token bucket — see QueueTextPacket comment.
	QueueTextPacket(0x9f20, reinterpret_cast<const uint8_t*>(&out),
	                static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleOutgoingConsider — OP_Consider (server → Trilogy client).
//
// EQEmu sends Consider_Struct: { uint32 playerid, targetid, faction (1-9
// post-swap), level (con color), int32 cur_hp, max_hp, uint8 pvpcon }.
// Trilogy expects: { int32 playerid, targetid, faction (hex), unknown_c[3],
//   unworthy }.
//
// After EQEmu's swap, factions 1-9 map 1:1 to EQClassic FACTION_VALUE.
// EQClassic encodes faction as: ally=0x500, warmly=0x300, kindly=0x200,
// amiable=0x100, indifferent=0x0, apprehensive=0xFFFFFFFF, dubious=0xFFFFFF00,
// threatening=0xFFFFFE00, scowls=0xFFFFFD00.
// ============================================================

void TrilogyClient::HandleOutgoingConsider(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Consider_Struct)) return;
	const auto* emu = reinterpret_cast<const ::Consider_Struct*>(app->pBuffer);

	static const int32_t kFactionHex[10] = {
		0x00000000, // 0 = unused
		0x00000500, // 1 = ally
		0x00000300, // 2 = warmly
		0x00000200, // 3 = kindly
		0x00000100, // 4 = amiable
		0x00000000, // 5 = indifferent
		(int32_t)0xFFFFFD00, // 6 = scowls (EQClassic)
		(int32_t)0xFFFFFE00, // 7 = threatening
		(int32_t)0xFFFFFF00, // 8 = dubious
		(int32_t)0xFFFFFFFF, // 9 = apprehensive (EQClassic)
	};

	uint32_t fac = emu->faction;
	int32_t hex_faction = (fac < 10) ? kFactionHex[fac] : 0;

	Trilogy::structs::Consider_Struct out{};
	memset(&out, 0, sizeof(out));
	out.playerid = static_cast<int32_t>(TranslateId(emu->playerid));
	out.targetid = static_cast<int32_t>(emu->targetid);
	out.faction  = hex_faction;

	m_tzs->SendToSession(m_session_key, 0x3721,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleExpUpdate — OP_ExpUpdate (server → Trilogy client).
//
// EQEmu ExpUpdate_Struct: { uint32 exp, uint32 aaxp } = 8 bytes.
// Trilogy ExpUpdate_Struct: { uint32 exp } = 4 bytes.
//
// The v29c client expects RAW cumulative experience here and computes the
// XP-bar fill internally using its own GetEXPForLevel formula — matches
// EQClassic\Zone\Source\client.cpp:656 (`eu->exp = set_exp`).  EQEmu pre-
// computes a 0-330 ratio in `emu->exp` (exp.cpp:874), but that's wrong for
// v29c: at level 2+ the small value underflows when the client subtracts the
// level's base exp, and the bar clips to either 0 or 100% (never the right
// percentage).  Use the cumulative value from m_pp.exp, which Client::SetEXP
// sets BEFORE dispatching this packet.
// ============================================================

void TrilogyClient::HandleExpUpdate(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::ExpUpdate_Struct)) return;

	Trilogy::structs::ExpUpdate_Struct out{};
	out.exp = GetEXP();

	m_tzs->SendToSession(m_session_key, 0x9921,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleLevelUpdate — OP_LevelUpdate (server → Trilogy client).
//
// EQEmu LevelUpdate_Struct: { uint32 level, level_old, exp } = 12 bytes.
// Trilogy LevelUpdate_Struct: { int8 level, can_delevel } = 2 bytes.
//
// can_delevel is misleadingly named — per the EQClassic LS struct doc, "if
// not equal to zero, the client allows the character to de-level.  If it is
// equal to zero, the client does NOT change the level of the played char."
// So 0 means "ignore this packet's level change entirely", and 1 means
// "apply the new level (including down-leveling)".  Always send 1, matching
// EQClassic\Zone\Source\client.cpp:721 (SendLevelUpdate sets it unconditionally).
// Earlier we set it to 0 on level-ups, which made v29c IGNORE the new level —
// the client kept its stale level, the XP-bar denominator stayed at the old
// level's exp range, and as soon as cumulative exp passed that range the bar
// clipped to 100% and refused to move on subsequent ExpUpdate packets.
// ============================================================

void TrilogyClient::HandleLevelUpdate(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::LevelUpdate_Struct)) return;
	const auto* emu = reinterpret_cast<const ::LevelUpdate_Struct*>(app->pBuffer);

	Trilogy::structs::LevelUpdate_Struct out{};
	out.level       = static_cast<int8_t>(emu->level);
	out.can_delevel = 1;

	m_tzs->SendToSession(m_session_key, 0x9821,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleMoneyOnCorpse — OP_MoneyOnCorpse (server → Trilogy client).
//
// EQEmu moneyOnCorpseStruct and Trilogy MoneyOnCorpse_Struct are
// byte-for-byte identical (20 bytes) — pass through directly.
// ============================================================

void TrilogyClient::HandleMoneyOnCorpse(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::moneyOnCorpseStruct)) return;

	m_tzs->SendToSession(m_session_key, 0x5020, app->pBuffer,
	                     static_cast<uint32_t>(sizeof(::moneyOnCorpseStruct)));
}

// ============================================================
// HandleOutgoingLootItem — OP_LootItem echo (server → Trilogy client).
//
// EQEmu echoes LootingItem_Struct after processing a loot request.
// The Trilogy client expects the same 16-byte struct on opcode 0xa020.
// Translate entity IDs (lootee/looter) using TranslateId.
// ============================================================

void TrilogyClient::HandleOutgoingLootItem(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::LootingItem_Struct)) return;
	const auto* emu = reinterpret_cast<const ::LootingItem_Struct*>(app->pBuffer);

	const uint32_t in_lootee = static_cast<uint32_t>(emu->lootee);
	const uint32_t in_looter = static_cast<uint32_t>(emu->looter);
	const uint32_t tr_lootee = TranslateId(in_lootee);
	const uint32_t tr_looter = TranslateId(in_looter);

	// Reverse the emu→wire loot slot mapping so the echo carries the same wire
	// slot the client sent us.  Scan the small (30-entry) map; emu_slot=0 sentinel
	// means unmapped so we fall back to slot_id-22 for the (rare) case where the
	// echo runs for a slot we never emitted (shouldn't happen, but defensive).
	int16_t echo_wire_slot = -1;
	for (int i = 0; i < kLootWireSlots; ++i) {
		if (m_loot_wire_to_emu[i] == emu->slot_id) {
			echo_wire_slot = static_cast<int16_t>(i + 1);
			break;
		}
	}
	if (echo_wire_slot < 0) {
		echo_wire_slot = static_cast<int16_t>(emu->slot_id - 22);
	}

	m_pending_echo_out = {};
	m_pending_echo_out.lootee    = static_cast<int32_t>(tr_lootee);
	m_pending_echo_out.looter    = static_cast<int32_t>(tr_looter);
	m_pending_echo_out.slot_id   = echo_wire_slot;
	m_pending_echo_out.auto_loot = static_cast<int32_t>(emu->auto_loot);
	m_pending_loot_echo = true;

	LogInfo("[TRILOGY-LOOT] HandleOutgoingLootItem queued: emu_slot={} → wire_slot={} lootee {}→{} looter {}→{} auto={}",
	        emu->slot_id, m_pending_echo_out.slot_id,
	        in_lootee, tr_lootee,
	        in_looter, tr_looter,
	        m_pending_echo_out.auto_loot);
	if (tr_lootee == 0 || tr_looter == 0) {
		LogInfo("[TRILOGY-LOOT] WARN HandleOutgoingLootItem: zero entity id after translate (lootee={} looter={}) — v29c may stall",
		        tr_lootee, tr_looter);
	}
	// Echo is sent by FlushPendingLootEcho() — either after the item delivery
	// packet (success path) or at OP_LootComplete (error / no-item path).
	// EQClassic sends item first then echo; we match that order.
}

void TrilogyClient::FlushPendingLootEcho()
{
	if (!m_pending_loot_echo) {
		LogInfo("[TRILOGY-LOOT] FlushPendingLootEcho: no pending echo (skipped)");
		return;
	}
	m_pending_loot_echo = false;
	LogInfo("[TRILOGY-LOOT] FlushPendingLootEcho sending 0xa020: wire_slot={} lootee={} looter={} auto={}",
	        m_pending_echo_out.slot_id,
	        m_pending_echo_out.lootee,
	        m_pending_echo_out.looter,
	        m_pending_echo_out.auto_loot);
	m_tzs->SendToSession(m_session_key, 0xa020,
	                     reinterpret_cast<const uint8_t*>(&m_pending_echo_out),
	                     static_cast<uint32_t>(sizeof(m_pending_echo_out)));
}

// ============================================================
// v29c cursor deferred-delivery — mirrors EQClassic SummonItem /
// summonedItems (Zone/Source/client.cpp:1614-1652 + client_process.cpp:1774).
//
// EnqueueOrSendSummonedItem: if the v29c client's cursor is already showing
// an item, we can't send another OP_SummonedItem — it will be silently
// dropped client-side and the loot appears to vanish (item lives on
// server-side cursor queue at DB 8000+ until next zone-in relocation).
// Instead we queue the pre-built wire bytes and dispatch them one at a
// time as the client clears its cursor via OP_MoveItem.
//
// OnClientCursorCleared: called from TrilogyZoneServer::HandleMoveItem
// after a from_wire=0 move (drop, equip, destroy, place-in-bag).  Clears
// the busy flag, and if a queued summon is waiting, pops it and sends it —
// this immediately re-arms busy so the next dequeue waits for another
// cursor clear.  Net effect: rapid multi-loot works and every item lands
// on cursor in sequence, matching EQClassic's SummonItem loop.
// ============================================================

void TrilogyClient::EnqueueOrSendSummonedItem(const uint8_t* wire, uint32_t size)
{
	if (!wire || size == 0) return;

	if (!m_client_cursor_busy) {
		m_client_cursor_busy = true;
		LogInfo("[TRILOGY-LOOT] Cursor free — sending OP_SummonedItem now ({} bytes), busy=true",
		        size);
		m_tzs->SendToSession(m_session_key, 0x7821, wire, size);
		return;
	}

	if (m_pending_summons.size() >= kMaxPendingSummons) {
		LogInfo("[TRILOGY-LOOT] Cursor queue full ({}); dropping OP_SummonedItem "
		        "(should never happen with normal loot volume)",
		        m_pending_summons.size());
		return;
	}
	m_pending_summons.emplace_back(wire, wire + size);
	LogInfo("[TRILOGY-LOOT] Cursor busy — queued OP_SummonedItem ({} bytes); queue_depth={}",
	        size, m_pending_summons.size());
}

void TrilogyClient::OnClientCursorCleared()
{
	m_client_cursor_busy = false;
	if (m_pending_summons.empty()) {
		return;
	}
	auto next = std::move(m_pending_summons.front());
	m_pending_summons.pop_front();
	m_client_cursor_busy = true;
	LogInfo("[TRILOGY-LOOT] Cursor cleared — dispatching next queued OP_SummonedItem "
	        "({} bytes); remaining_queue={}",
	        static_cast<unsigned>(next.size()),
	        m_pending_summons.size());
	m_tzs->SendToSession(m_session_key, 0x7821, next.data(),
	                     static_cast<uint32_t>(next.size()));
}

// ============================================================
// Corpse loot slot renumbering + retransmit dedup.
// See header for full rationale.
// ============================================================

void TrilogyClient::ResetLootSession()
{
	for (int i = 0; i < kLootWireSlots; ++i) m_loot_wire_to_emu[i] = 0;
	m_next_loot_wire_slot = 1;
	m_last_loot_lootee    = 0;
	m_last_loot_wire_slot = -1;
	m_last_loot_ts_ms     = 0;
}

int16_t TrilogyClient::AssignLootWireSlot(int16_t emu_slot)
{
	if (m_next_loot_wire_slot > kLootWireSlots) {
		LogInfo("[TRILOGY-LOOT] AssignLootWireSlot: v29c 30-slot corpse array full, "
		        "dropping emu_slot={} (over-cap items are invisible to the client)",
		        emu_slot);
		return 0;
	}
	const int16_t wire = m_next_loot_wire_slot++;
	m_loot_wire_to_emu[wire - 1] = emu_slot;
	return wire;
}

int16_t TrilogyClient::LookupLootEmuSlot(int16_t wire_slot) const
{
	if (wire_slot < 1 || wire_slot > kLootWireSlots) return -1;
	const int16_t emu = m_loot_wire_to_emu[wire_slot - 1];
	return (emu == 0) ? -1 : emu;
}

bool TrilogyClient::IsDuplicateLootItem(uint32_t lootee, int16_t wire_slot)
{
	const uint64_t now_ms = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::now().time_since_epoch()).count());

	if (lootee == m_last_loot_lootee && wire_slot == m_last_loot_wire_slot &&
	    (now_ms - m_last_loot_ts_ms) <= kLootDedupWindowMs) {
		LogInfo("[TRILOGY-LOOT] IsDuplicateLootItem: dropping ARQ retransmit "
		        "lootee={} wire_slot={} age_ms={} (window={}ms)",
		        lootee, wire_slot, (now_ms - m_last_loot_ts_ms), kLootDedupWindowMs);
		return true;
	}

	m_last_loot_lootee    = lootee;
	m_last_loot_wire_slot = wire_slot;
	m_last_loot_ts_ms     = now_ms;
	return false;
}

// ============================================================
// Bot ^invgive cursor bridge — thin wrappers around the TrilogyZoneServer
// helpers so bot_commands/inventory.cpp can call them via Client*.
// See trilogy_zone.cpp:MaterializeCursorForBotTrade for the full rationale.
// ============================================================
int TrilogyClient::MaterializeCursorForBotTrade()
{
	return m_tzs ? m_tzs->MaterializeCursorForBotTrade(this) : -1;
}

void TrilogyClient::FinalizeCursorAfterBotTrade(int src_db)
{
	if (m_tzs) m_tzs->FinalizeCursorAfterBotTrade(this, src_db);
}

// ============================================================
// HandleGroundSpawn — translate OP_GroundSpawn (EQEmu internal) to the
// EQClassic object spawn packet (opcode 0x3520, 240 bytes).
//
// Handles BOTH player-dropped ground items AND static world objects /
// tradeskill containers (forge, kiln, loom, oven, brew barrel, …), since
// EQEmu serializes both through OP_GroundSpawn / Object_Struct.
//
// EQEmu's Object_Struct carries object_name (IT*_ACTORDEF), position, drop_id,
// and object_type.  The EQClassic client renders the 3D model from objectname
// and uses dropid to identify the object for interaction (OP_PickupItem 0x3620).
//
// EQEmu object_type values (object.h ObjectTypes): 0 = StaticLocked scenery and
// player-dropped items (both leave the wire object_type at 0), 1 = Temporary,
// and 10+ = tradeskill containers (ToolBox=10 … Forge=17 … PotteryKiln=22 …).
// A wire object_type >= ToolBox marks a crafting container.
//
// CRITICAL: a crafting/world container needs MORE than a dropped item to render.
// EQClassic's Object::CreateSpawnPacket (objecttype==0 path) sets a sentinel
// itemid (17005) and several flag bytes to 0x01; a dropped item leaves those
// zero.  Without them the client will NOT render the container model — which is
// why setting only the "type" byte did nothing.  We replicate that path here.
// ============================================================

void TrilogyClient::HandleGroundSpawn(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Object_Struct)) return;
	const auto* emu = reinterpret_cast<const ::Object_Struct*>(app->pBuffer);

	// EQClassic Object_Struct layout (240 bytes):
	//   [0]   int8[4]  unknown_4b
	//   [4]   int8[4]  client_address
	//   [8]   int32    itemid          (0 = let client derive from model)
	//   [12]  int32    dropid          (entity ID used for pickup)
	//   [16]  int8[24] unknown_24
	//   [40]  float    ypos
	//   [44]  float    xpos
	//   [48]  float    zpos
	//   [52]  float    heading
	//   [0]   int8[4]  unknown_4b       (0x01 x4 for world containers)
	//   [8]   int32    itemid          (0 for dropped item; 17005 for world container)
	//   [12]  int32    dropid          (entity ID used for pickup/interaction)
	//   [40]  float    ypos
	//   [44]  float    xpos
	//   [48]  float    zpos
	//   [52]  float    heading
	//   [56]  char[16] objectname      (e.g. "IT63_ACTORDEF\0")
	//   [106] int8[6]  unknown_6        (0x01 x6 for world containers)
	//   [122] int8[5]  unknown_5        (0x01 x5 for world containers)
	//   [232] int8[4]  unknown_12       (0x01; EQClassic overruns into icon_nr/type)
	//   [236] int16    icon_nr
	//   [238] int16    type             (1 = OT_DROPPEDITEM)
	//   total = 240 bytes
	static constexpr uint32_t CLASSIC_OBJ_SIZE = 240;
	static constexpr int16_t  OT_DROPPEDITEM   = 1;
	uint8_t buf[CLASSIC_OBJ_SIZE];
	memset(buf, 0, sizeof(buf));

	*reinterpret_cast<int32_t*>(buf + 12)  = static_cast<int32_t>(emu->drop_id);
	*reinterpret_cast<float*>  (buf + 40)  = emu->y;
	*reinterpret_cast<float*>  (buf + 44)  = emu->x;
	*reinterpret_cast<float*>  (buf + 48)  = emu->z;
	*reinterpret_cast<float*>  (buf + 52)  = emu->heading;
	strncpy(reinterpret_cast<char*>(buf + 56), emu->object_name, 15);
	buf[56 + 15] = '\0';

	if (emu->object_type >= ObjectTypes::ToolBox) {
		// World / tradeskill container — replicate EQClassic Object::CreateSpawnPacket
		// (objecttype==0 path) verbatim.  Without the sentinel itemid and these 0x01
		// flag bytes the client does not render the container model.
		*reinterpret_cast<int32_t*>(buf + 8) = 17005; // sentinel world-object item id
		memset(buf + 0,   0x01, 4);  // unknown_4b
		memset(buf + 106, 0x01, 6);  // unknown_6
		memset(buf + 122, 0x01, 5);  // unknown_5
		// EQClassic writes 8 bytes to the 4-byte unknown_12, deliberately overrunning
		// into icon_nr/type; replicated exactly (safe within the 240-byte buffer).
		memset(buf + 232, 0x01, 8);

		// Diagnostic: hex-dump the full spawn packet so we can compare against
		// EQClassic Object::CreateSpawnPacket byte-for-byte.  Without this we
		// can't tell whether the v29c client is rejecting the spawn shape (and
		// therefore treating the click as falling through to a default fallback
		// like the inventory window).
		LogInfo("[TRILOGY-TS] HandleGroundSpawn TRADESKILL drop_id={} type={} object_name='{}' (240B)",
		        emu->drop_id, emu->object_type, emu->object_name);
		const size_t row_n = 32;
		for (size_t off = 0; off < CLASSIC_OBJ_SIZE; off += row_n) {
			std::string line = fmt::format("[TRILOGY-TS] spawn @0x{:03x}:", off);
			const size_t end = std::min(off + row_n, (size_t)CLASSIC_OBJ_SIZE);
			for (size_t i = off; i < end; ++i)
				line += fmt::format(" {:02x}", buf[i]);
			LogInfo("{}", line);
		}
	} else {
		// Player-dropped ground item (unchanged, working path).
		*reinterpret_cast<int16_t*>(buf + 238) = OT_DROPPEDITEM;
	}

	if (m_is_zoning) {
		if (m_deferred_spawns.size() < kMaxDeferredSpawns) {
			m_deferred_spawns.emplace_back(uint16_t{0x3520},
				std::vector<uint8_t>(buf, buf + sizeof(buf)));
		} else {
			LogError("[TrilogyClient] HandleGroundSpawn: deferred queue FULL ({}), DROPPING object '{}'",
			         kMaxDeferredSpawns, emu->object_name);
		}
		return;
	}

	m_tzs->SendToSession(m_session_key, 0x3520,
	                     buf, static_cast<uint32_t>(sizeof(buf)));
}

// ============================================================
// HandleMoveDoor — translate EQEmu OP_MoveDoor (MoveDoor_Struct, 2 bytes)
// to the OP_OpenDoor packet (0x8e20) the Trilogy client expects.
//
// The wire layout is identical: { uint8 doorid, uint8 action }.  The Trilogy
// client uses the same action convention as EQEmu/Titanium — OPEN_DOOR=0x02,
// CLOSE_DOOR=0x03 (and the OPEN_INVDOOR/CLOSE_INVDOOR swap for inverted doors),
// which is exactly what EQEmu's Doors::HandleClick already produces.  This was
// verified against EQMacEmuTrilogy (doors.cpp), which targets the same client
// and sends OPEN_DOOR=0x02 unaltered.  So the action byte is forwarded as-is —
// no inversion (an earlier 0x02<->0x03 swap caused 2-3 clicks to open a door,
// because it desynced the client's door state from the server's).
//
// Dedup uses a short TTL (kDoorDedupWindowMs) — see the m_last_door_action
// comment in the header for why a permanent dedup breaks normal door reuse
// after server-side auto-close.
// ============================================================

void TrilogyClient::HandleMoveDoor(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::MoveDoor_Struct)) return;
	const auto* emu = reinterpret_cast<const ::MoveDoor_Struct*>(app->pBuffer);

	// DoorOpen_Struct == MoveDoor_Struct on the wire: { int8 doorid, int8 action }.
	const uint8_t doorid = static_cast<uint8_t>(emu->doorid);
	const uint8_t action = static_cast<uint8_t>(emu->action);

	const auto now = std::chrono::steady_clock::now();
	auto it = m_last_door_action.find(doorid);
	if (it != m_last_door_action.end() && it->second.first == action) {
		const auto elapsed_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				now - it->second.second).count();
		if (elapsed_ms < static_cast<int64_t>(kDoorDedupWindowMs)) {
			LogInfo("[TrilogyDiag] 8E20 MoveDoor dedup doorid={} action={} elapsed_ms={}",
			        static_cast<int>(doorid), static_cast<int>(action),
			        static_cast<long long>(elapsed_ms));
			return;
		}
	}
	m_last_door_action[doorid] = std::make_pair(action, now);

	LogInfo("[TrilogyDiag] 8E20 MoveDoor doorid={} action={}",
	        static_cast<int>(doorid), static_cast<int>(action));

	uint8_t out[2];
	out[0] = doorid;
	out[1] = action;
	m_tzs->SendToSession(m_session_key, 0x8e20, out, sizeof(out));
}

// ============================================================
// HandleOutgoingShopRequest — OP_ShopRequest (server → Trilogy client).
//
// EQEmu's Handle_OP_ShopRequest replies with MerchantClick_Struct carrying the
// open/close command and a price multiplier `rate` = 1/(buy_cost_mod * CalcPriceMod).
// Translate to the 16-byte Trilogy Merchant_Click_Struct (opcode 0x0b20):
//   unknown[0] = action (1 open / 0 close), unknown[1] = 0x03 (EQClassic constant),
//   pricemultiplier = rate.  The client shows buy = item.price * mult and
//   sell = item.price / mult, which our pricing in HandleItemPacket / the zone
//   server's buy/sell handlers is built around.
//
// A fresh open also resets this client's merchant-window cache; the item rows
// follow as OP_ItemPacket(ItemPacketMerchant) → 0x0c20 from BulkSendMerchantInventory.
// ============================================================

void TrilogyClient::HandleOutgoingShopRequest(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::MerchantClick_Struct)) return;
	const auto* mc = reinterpret_cast<const ::MerchantClick_Struct*>(app->pBuffer);

	m_merchant_npc_id = static_cast<uint16_t>(mc->npc_id);
	m_merchant_rate   = (mc->rate > 0.0001f) ? mc->rate : 1.0f;
	m_merchant_window.clear(); // items for this open arrive next as 0x0c20

	Trilogy::structs::Merchant_Click_Struct out{};
	memset(&out, 0, sizeof(out));
	out.entityid        = static_cast<int32_t>(mc->npc_id);
	out.playerid        = static_cast<int32_t>(GetID());
	out.unknown[0]      = static_cast<int8_t>(mc->command); // 1 = open, 0 = close
	out.unknown[1]      = 0x03;
	out.pricemultiplier = m_merchant_rate;

	m_tzs->SendToSession(m_session_key, 0x0b20,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleOutgoingReadBook — strip EQEmu's BookText_Struct header so the v29c
// client's book GUI receives just the raw text (its on-wire format).
//
// Internal layout (BookText_Struct, eq_packet_structs.h):
//   uint8 window; uint8 type; int16 invslot; int32 target_id;
//   int8  can_cast; int8 can_scribe; char booktext[]; (variable)
// The first 10 bytes are header; everything after is the text body, already
// language-garbled by Client::ReadBook based on the player's language skill.
// EQClassic ReadBook (Zone/Source/client.cpp:2467) sends exactly that body
// with no header — `'`` marks newlines in the GUI.
// ============================================================
void TrilogyClient::HandleOutgoingReadBook(const EQApplicationPacket* app)
{
	if (!app) return;
	// EQEmu allocates `length + sizeof(BookText_Struct)` bytes — header is
	// the first 10 bytes (window, type, invslot, target_id, can_cast, can_scribe);
	// booktext[1] gives a 1-byte trailing slot that's always zero from the
	// EQApplicationPacket ctor.  Strip both the header and the trailing slot
	// so we send exactly the text the v29c book GUI expects.
	if (app->size <= sizeof(::BookText_Struct)) return;
	const uint8_t* text = app->pBuffer + 10;
	const uint32_t len  = app->size - sizeof(::BookText_Struct);
	m_tzs->SendToSession(m_session_key, 0xce20, text, len);
}

// ============================================================
// HandleOutgoingRezzRequest — translate the 228-byte modern Resurrect_Struct
// to the 160-byte v29c layout and fire it out at opcode 0x2a21 so the client
// pops the resurrection window.
//
// The engine (Corpse::CastRezz → worldserver.RezzPlayer → world routes to
// owner's zone → WorldServer::HandleMessage:ServerOP_RezzPlayer) hands us the
// modern struct via QueuePacket.  Handle_OP_RezzAnswer will fire back at
// 0x9b21 with the same 160B layout and the client's chosen action field.
//
// Field mapping (modern → v29c):
//   zone_id (uint16)          -> zoneName[16]  via zone_store.GetZoneName
//   y/x/z   (float)           -> y/x/z         (same order)
//   your_name[64]             -> targetName[30]   (v29c popup uses this)
//   rezzer_name[64]           -> casterName[30]
//   corpse_name[64]           -> corpseName[28]
//   spellid  (uint32)         -> spellID       (uint16; rez spells fit)
// unknown fields left zero — v29c ignores them.
// corpseEntityID is not carried on the outbound path (the caster's zone owns
// the corpse lookup); it's echoed back verbatim on 0x9b21 which we ignore
// because pending-rez state is server-side (SetPendingRezzData).
// ============================================================
void TrilogyClient::HandleOutgoingRezzRequest(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Resurrect_Struct)) return;

	const auto* r_in = reinterpret_cast<const ::Resurrect_Struct*>(app->pBuffer);

	Trilogy::structs::Resurrect_Struct out{};
	// corpseEntityID left zero; client only uses targetName to route the popup.
	out.corpseEntityID = 0;

	const char* zone_short = zone_store.GetZoneName(r_in->zone_id, false);
	if (zone_short && *zone_short) {
		strn0cpy(out.zoneName, zone_short, sizeof(out.zoneName));
	}

	out.y         = r_in->y;
	out.x         = r_in->x;
	out.z         = r_in->z;
	out.fullGMRez = 0;
	out.spellID   = static_cast<uint16_t>(r_in->spellid);

	strn0cpy(out.targetName, r_in->your_name,   sizeof(out.targetName));
	strn0cpy(out.casterName, r_in->rezzer_name, sizeof(out.casterName));
	strn0cpy(out.corpseName, r_in->corpse_name, sizeof(out.corpseName));

	LogSpells(
		"[TrilogyRezz] send OP_RezzRequest target={} caster={} spell={} corpse={} zone={} pos=({:.1f},{:.1f},{:.1f})",
		out.targetName, out.casterName, out.spellID, out.corpseName,
		out.zoneName, out.x, out.y, out.z);

	m_tzs->SendToSession(m_session_key, 0x2a21,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// Group translators (server → Trilogy client)
//
// EQEmu internal structs use 64-byte names; the v29c wire format uses 32-byte
// names.  All copies use strncpy + an explicit final-byte zero so a truncated
// 32+ char name still terminates cleanly inside the wire field.
// ============================================================

namespace {
inline void CopyTrilogyName(char* dst, size_t dst_size, const char* src)
{
	if (!dst || dst_size == 0) return;
	if (src) {
		strncpy(dst, src, dst_size - 1);
		dst[dst_size - 1] = '\0';
	} else {
		dst[0] = '\0';
	}
}

// Hex dump for group packet diagnostics — caps at 256 bytes so we never spew
// huge GroupUpdate_Struct (228B) blobs but always cover the full payload.
inline std::string HexDumpBytes(const void* data, uint32_t len)
{
	if (!data || len == 0) return std::string{};
	const uint8_t* p = static_cast<const uint8_t*>(data);
	const uint32_t cap = len > 256 ? 256u : len;
	std::string hex;
	hex.reserve(cap * 3 + 8);
	for (uint32_t i = 0; i < cap; ++i) {
		hex += fmt::format("{:02X} ", p[i]);
	}
	if (len > cap) hex += "...";
	return hex;
}
}

// HandleOutgoingGroupInvite — internal GroupGeneric_Struct (128B: name1/name2)
// → wire OP_GroupInvite2 (0x4020) GroupInvite_Struct (91B = 30 + 30 + 31).
//
// EQEmu's Handle_OP_GroupInvite2 sends GroupGeneric_Struct{ name1=invitee,
// name2=inviter } via QueuePacket.  EQClassic LS forwards the inviter's
// original opcode UNCHANGED — when the v29c client sent OP_GroupInvite2
// (0x4020), the invitee also receives it on 0x4020, which is what triggers
// the invitee's group-window button to switch from "Disband" to "Follow".
// Sending it on 0x3e20 (the other valid invite opcode) does NOT flip that
// button in practice, confirmed by live test 2026-06-18.  We send on 0x4020
// regardless of which opcode the inviter used; the v29c popup-receive path
// accepts it either way and the button flips reliably.
void TrilogyClient::HandleOutgoingGroupInvite(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::GroupGeneric_Struct)) return;
	const auto* emu = reinterpret_cast<const ::GroupGeneric_Struct*>(app->pBuffer);

	Trilogy::structs::GroupInvite_Struct out{};
	CopyTrilogyName(out.invitee_name, sizeof(out.invitee_name), emu->name1);
	CopyTrilogyName(out.inviter_name, sizeof(out.inviter_name), emu->name2);
	// unknown[] stays zero.

	LogInfo("[Trilogy][Group] -> OP_GroupInvite2 (0x4020) invitee=[{}] inviter=[{}] size={} bytes=[{}]",
	        out.invitee_name, out.inviter_name, sizeof(out), HexDumpBytes(&out, sizeof(out)));
	m_tzs->SendToSession(m_session_key, 0x4020,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// HandleOutgoingGroupFollow — internal GroupGeneric_Struct or GroupFollow_Struct
// → wire OP_GroupFollow (0x3d20) GroupFollow_Struct (64B).
//
// Not normally pushed outbound by the engine (Follow is client→server only),
// but we translate defensively in case quest/world code echoes it.
void TrilogyClient::HandleOutgoingGroupFollow(const EQApplicationPacket* app)
{
	if (!app) return;

	const char* leader  = nullptr;
	const char* invited = nullptr;

	if (app->size >= sizeof(::GroupFollow_Struct)) {
		const auto* emu = reinterpret_cast<const ::GroupFollow_Struct*>(app->pBuffer);
		leader  = emu->name1;
		invited = emu->name2;
	} else if (app->size >= sizeof(::GroupGeneric_Struct)) {
		const auto* emu = reinterpret_cast<const ::GroupGeneric_Struct*>(app->pBuffer);
		leader  = emu->name1;
		invited = emu->name2;
	} else {
		return;
	}

	Trilogy::structs::GroupFollow_Struct out{};
	CopyTrilogyName(out.leader,  sizeof(out.leader),  leader);
	CopyTrilogyName(out.invited, sizeof(out.invited), invited);

	LogInfo("[Trilogy][Group] -> OP_GroupFollow (0x4220) leader=[{}] invited=[{}] size={} bytes=[{}]",
	        out.leader, out.invited, sizeof(out), HexDumpBytes(&out, sizeof(out)));
	m_tzs->SendToSession(m_session_key, 0x4220,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// HandleOutgoingGroupCancelInvite — internal GroupCancel_Struct (129B) or
// GroupGeneric_Struct (128B) → wire OP_GroupCancelInvite (0x4120)
// GroupInviteDecline_Struct (61B; names are [30] not [32], same trap as Invite/Follow).
void TrilogyClient::HandleOutgoingGroupCancelInvite(const EQApplicationPacket* app)
{
	if (!app) return;

	Trilogy::structs::GroupInviteDecline_Struct out{};

	if (app->size >= sizeof(::GroupCancel_Struct)) {
		const auto* emu = reinterpret_cast<const ::GroupCancel_Struct*>(app->pBuffer);
		CopyTrilogyName(out.leader, sizeof(out.leader), emu->name1);
		CopyTrilogyName(out.leaver, sizeof(out.leaver), emu->name2);
		// EQEmu's `toggle` is the same semantic as v29c `action` (1/2/3).
		out.action = emu->toggle ? emu->toggle : 3; // default reject
	} else if (app->size >= sizeof(::GroupGeneric_Struct)) {
		const auto* emu = reinterpret_cast<const ::GroupGeneric_Struct*>(app->pBuffer);
		CopyTrilogyName(out.leader, sizeof(out.leader), emu->name1);
		CopyTrilogyName(out.leaver, sizeof(out.leaver), emu->name2);
		out.action = 3;
	} else {
		return;
	}

	LogInfo("[Trilogy][Group] -> OP_GroupCancelInvite (0x4120) leader=[{}] leaver=[{}] action={} size={} bytes=[{}]",
	        out.leader, out.leaver, out.action, sizeof(out), HexDumpBytes(&out, sizeof(out)));
	m_tzs->SendToSession(m_session_key, 0x4120,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// HandleOutgoingGroupDisband — translate to v29c-style "you leave" via
// OP_GroupUpdate (0x2640) with action=4.  EQEmu doesn't normally emit
// OP_GroupDisband outbound (leave events flow through OP_GroupUpdate), but
// keep this here so any path that does push it gets the right wire effect:
// clearing the v29c group window.
void TrilogyClient::HandleOutgoingGroupDisband(const EQApplicationPacket* app)
{
	if (!app) return;

	Trilogy::structs::GroupUpdate_Struct out{};
	CopyTrilogyName(out.yourname, sizeof(out.yourname), GetName());
	out.action = 4; // "you leave"

	LogInfo("[Trilogy][Group] -> OP_GroupUpdate (0x2640) action=4 (disband/you leave) size={} bytes=[{}]",
	        sizeof(out), HexDumpBytes(&out, sizeof(out)));
	m_tzs->SendToSession(m_session_key, 0x2640,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// HandleOutgoingGroupUpdate — the main group bus.
//
// EQEmu re-uses OP_GroupUpdate for several payloads, all 452B (or 836B with
// leadership AAs). At offset 0/4 they share action(uint32) + yourname[64].
// At offset 68 they diverge:
//   • GroupJoin_Struct: single membername[64] (the member acted on)
//   • GroupUpdate_Struct: membername[5][64] (full roster) + leadersname[64]
//
// Both structs put the relevant "other" name at offset 68, so reading it as
// GroupUpdate_Struct lets us pull `membername[0]` whether the source was a
// per-member event or a roster blob.
//
// Wire action codes (per EQMacEmuTrilogy ENCODE(OP_GroupUpdate)):
//   0 = ADD_MEMBER     — "X joins"
//   1 = NEW_LEADER     — "X is now leader"
//   3 = REMOVE_MEMBER  — "X leaves"
//   4 = DISBAND_YOU    — "You leave / group disbanded"
//
// Mapping from EQEmu action → v29c action (mirrors EQMacEmuTrilogy
// trilogy.cpp:2381 ENCODE(OP_GroupUpdate)):
//   groupActJoin(0)          → 0  (with othername = membername[0])
//   groupActLeave(1)         → 3  (with othername = membername[0])
//   groupActDisband(6)       → 4  (with othername = yourname)
//   groupActUpdate(7)        → 0  (treat as add — refresh trigger)
//   groupActMakeLeader(8)    → 1  (with othername = leadersname)
//   groupActInviteInitial(9) → 0  (the group-creation packet to yourself)
//   groupActAAUpdate(10)     → drop (v29c has no leadership AAs)
//
// v29c maintains its own group roster locally and uses these packets as
// per-event deltas (matches EQClassic/Zone/Source/groups.cpp:91-141
// Group::AddMember — one packet per (receiver, sender) pair, no roster).
// We do NOT populate the membername[] array.
//
// Opcode: 0x2640 (EQClassic/Common/Include/eq_opcodes.h:68) — NOT 0x2620
// (LS variant).  Sending on 0x2620 makes v29c silently discard the packet
// and the group window never paints.
void TrilogyClient::HandleOutgoingGroupUpdate(const EQApplicationPacket* app)
{
	if (!app) return;

	if (app->size != sizeof(::GroupJoin_Struct) &&
	    app->size != sizeof(::GroupUpdate_Struct) &&
	    app->size != sizeof(::GroupUpdate2_Struct)) {
		LogInfo("[Trilogy][Group] OP_GroupUpdate dropped — unexpected size {}", app->size);
		return;
	}

	// Read action + yourname from the shared header.
	const uint32_t emu_action = *reinterpret_cast<const uint32_t*>(app->pBuffer);
	const char* yourname      = reinterpret_cast<const char*>(app->pBuffer + 4);

	if (emu_action == groupActAAUpdate) {
		// Leadership AAs are a SoL+ concept; v29c has no UI for them.
		return;
	}

	// `membername[0]` (offset 68) overlaps both GroupJoin_Struct::membername
	// and GroupUpdate_Struct::membername[0], so it gives us the "other"
	// person in either layout.
	const char* other_member = reinterpret_cast<const char*>(app->pBuffer + 68);

	Trilogy::structs::GroupUpdate_Struct out{};
	CopyTrilogyName(out.yourname, sizeof(out.yourname), yourname);

	switch (emu_action) {
	case groupActJoin:
	case groupActUpdate:
	case groupActInviteInitial:
		CopyTrilogyName(out.othername, sizeof(out.othername), other_member);
		out.action = 0; // ADD_MEMBER
		break;
	case groupActLeave:
		CopyTrilogyName(out.othername, sizeof(out.othername), other_member);
		out.action = 3; // REMOVE_MEMBER
		break;
	case groupActDisband:
		// Sent to the leaving/disbanded member only: tell v29c "you leave".
		CopyTrilogyName(out.othername, sizeof(out.othername), yourname);
		out.action = 4; // DISBAND_YOU
		break;
	case groupActMakeLeader: {
		// GroupUpdate_Struct (452B/836B) has leadersname at offset 388;
		// GroupJoin_Struct does not (offset 388 falls in its unknown blob),
		// but ChangeLeader emits the GroupJoin_Struct path with the new
		// leader's name in `membername` (offset 68) instead.  Prefer the
		// roster's leadersname when present, fall back to membername.
		if (app->size >= sizeof(::GroupUpdate_Struct)) {
			const char* leadersname =
			    reinterpret_cast<const char*>(app->pBuffer + 388);
			if (leadersname[0] != '\0') {
				CopyTrilogyName(out.othername, sizeof(out.othername), leadersname);
			} else {
				CopyTrilogyName(out.othername, sizeof(out.othername), other_member);
			}
		} else {
			CopyTrilogyName(out.othername, sizeof(out.othername), other_member);
		}
		out.action = 1; // NEW_LEADER
		break;
	}
	default:
		LogInfo("[Trilogy][Group] OP_GroupUpdate emu_action={} unmapped, dropping", emu_action);
		return;
	}

	LogInfo("[Trilogy][Group] -> OP_GroupUpdate (0x2640) emu_action={} v29c_action={} yourname=[{}] othername=[{}] size={} bytes=[{}]",
	        emu_action, out.action, out.yourname, out.othername,
	        sizeof(out), HexDumpBytes(&out, sizeof(out)));
	m_tzs->SendToSession(m_session_key, 0x2640,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// Group translators (client → server)
//
// Convert v29c wire structs (32B names) to EQEmu internal form (64B names)
// and dispatch to the existing Handle_OP_Group* methods.  Each preserves the
// original sender semantics: invite carries (invitee, inviter), follow
// carries (leader, invited), cancel carries (leader, leaver, action).
// ============================================================

void TrilogyClient::HandleIncomingGroupInvite(const uint8_t* data, uint32_t len)
{
	LogInfo("[Trilogy][Group] <- OP_GroupInvite (0x3e20) raw len={} bytes=[{}]",
	        len, HexDumpBytes(data, len));
	if (!data || len < sizeof(Trilogy::structs::GroupInvite_Struct)) {
		LogInfo("[Trilogy][Group] <- OP_GroupInvite (0x3e20) dropped (len={})", len);
		return;
	}
	const auto* wire = reinterpret_cast<const Trilogy::structs::GroupInvite_Struct*>(data);

	EQApplicationPacket pkt(OP_GroupInvite, sizeof(::GroupInvite_Struct));
	auto* gi = reinterpret_cast<::GroupInvite_Struct*>(pkt.pBuffer);
	memset(gi, 0, sizeof(::GroupInvite_Struct));
	strn0cpy(gi->invitee_name, wire->invitee_name, sizeof(gi->invitee_name));
	strn0cpy(gi->inviter_name, wire->inviter_name, sizeof(gi->inviter_name));

	LogInfo("[Trilogy][Group] <- OP_GroupInvite (0x3e20) invitee=[{}] inviter=[{}]",
	         gi->invitee_name, gi->inviter_name);
	Handle_OP_GroupInvite(&pkt);
}

void TrilogyClient::HandleIncomingGroupInvite2(const uint8_t* data, uint32_t len)
{
	LogInfo("[Trilogy][Group] <- OP_GroupInvite2 (0x4020) raw len={} bytes=[{}]",
	        len, HexDumpBytes(data, len));
	if (!data || len < sizeof(Trilogy::structs::GroupInvite_Struct)) {
		LogInfo("[Trilogy][Group] <- OP_GroupInvite2 (0x4020) dropped (len={})", len);
		return;
	}
	const auto* wire = reinterpret_cast<const Trilogy::structs::GroupInvite_Struct*>(data);

	// Forge as OP_GroupInvite2 so Handle_OP_GroupInvite2's app->GetOpcode()
	// branch (which rewraps to OP_GroupInvite before forwarding to the invitee)
	// takes the right path.
	EQApplicationPacket pkt(OP_GroupInvite2, sizeof(::GroupInvite_Struct));
	auto* gi = reinterpret_cast<::GroupInvite_Struct*>(pkt.pBuffer);
	memset(gi, 0, sizeof(::GroupInvite_Struct));
	strn0cpy(gi->invitee_name, wire->invitee_name, sizeof(gi->invitee_name));
	strn0cpy(gi->inviter_name, wire->inviter_name, sizeof(gi->inviter_name));

	LogInfo("[Trilogy][Group] <- OP_GroupInvite2 (0x4020) invitee=[{}] inviter=[{}]",
	         gi->invitee_name, gi->inviter_name);

	// Pre-check whether the invitee is a Bot.  For player-to-player invites,
	// the invitee's client will reply with OP_GroupFollow (0x4220) and the
	// EQEmu engine then echoes that OP_GroupFollow back to the inviter — that
	// echo is how the v29c client learns it is the new group's leader
	// (without it, the client's local "am I the leader?" check fails and
	// the next OP_GroupInvite2 is blocked with "Only a leader can invite
	// more members").  Bot::ProcessBotGroupInvite skips the Follow step
	// entirely and just appends the bot to the group server-side, so the
	// inviter never receives an OP_GroupFollow.  Forge one here so v29c sees
	// the same wire sequence it would for a player invite that auto-accepted.
	Mob* invitee_mob = entity_list.GetMob(gi->invitee_name);
	const bool invitee_is_bot = invitee_mob && invitee_mob->IsBot();

	Handle_OP_GroupInvite2(&pkt);

	if (invitee_is_bot) {
		Trilogy::structs::GroupFollow_Struct out{};
		CopyTrilogyName(out.leader,  sizeof(out.leader),  gi->inviter_name);
		CopyTrilogyName(out.invited, sizeof(out.invited), gi->invitee_name);
		LogInfo("[Trilogy][Group] -> OP_GroupFollow (0x4220) forged bot-accept leader=[{}] invited=[{}] size={} bytes=[{}]",
		        out.leader, out.invited, sizeof(out), HexDumpBytes(&out, sizeof(out)));
		m_tzs->SendToSession(m_session_key, 0x4220,
		                     reinterpret_cast<const uint8_t*>(&out),
		                     static_cast<uint32_t>(sizeof(out)));
	}
}

void TrilogyClient::HandleIncomingGroupFollow(const uint8_t* data, uint32_t len)
{
	LogInfo("[Trilogy][Group] <- OP_GroupFollow (0x4220) raw len={} bytes=[{}]",
	        len, HexDumpBytes(data, len));
	if (!data || len < sizeof(Trilogy::structs::GroupFollow_Struct)) {
		LogInfo("[Trilogy][Group] <- OP_GroupFollow (0x4220) dropped (len={})", len);
		return;
	}
	const auto* wire = reinterpret_cast<const Trilogy::structs::GroupFollow_Struct*>(data);

	// Handle_OP_GroupFollow expects GroupGeneric_Struct { name1=inviter,
	// name2=invitee }.
	EQApplicationPacket pkt(OP_GroupFollow, sizeof(::GroupGeneric_Struct));
	auto* gg = reinterpret_cast<::GroupGeneric_Struct*>(pkt.pBuffer);
	memset(gg, 0, sizeof(::GroupGeneric_Struct));
	strn0cpy(gg->name1, wire->leader,  sizeof(gg->name1));
	strn0cpy(gg->name2, wire->invited, sizeof(gg->name2));

	LogInfo("[Trilogy][Group] <- OP_GroupFollow (0x3d20) inviter=[{}] invitee=[{}]",
	         gg->name1, gg->name2);
	Handle_OP_GroupFollow(&pkt);
}

void TrilogyClient::HandleIncomingGroupCancelInvite(const uint8_t* data, uint32_t len)
{
	LogInfo("[Trilogy][Group] <- OP_GroupCancelInvite (0x4120) raw len={} bytes=[{}]",
	        len, HexDumpBytes(data, len));
	if (!data || len < sizeof(Trilogy::structs::GroupInviteDecline_Struct)) {
		LogInfo("[Trilogy][Group] <- OP_GroupCancelInvite (0x4120) dropped (len={})", len);
		return;
	}
	const auto* wire = reinterpret_cast<const Trilogy::structs::GroupInviteDecline_Struct*>(data);

	// Handle_OP_GroupCancelInvite expects GroupCancel_Struct {name1=inviter,
	// name2=leaver, toggle=action}.
	EQApplicationPacket pkt(OP_GroupCancelInvite, sizeof(::GroupCancel_Struct));
	auto* gc = reinterpret_cast<::GroupCancel_Struct*>(pkt.pBuffer);
	memset(gc, 0, sizeof(::GroupCancel_Struct));
	strn0cpy(gc->name1, wire->leader, sizeof(gc->name1));
	strn0cpy(gc->name2, wire->leaver, sizeof(gc->name2));
	gc->toggle = wire->action;

	LogInfo("[Trilogy][Group] <- OP_GroupCancelInvite (0x4120) inviter=[{}] leaver=[{}] action={}",
	         gc->name1, gc->name2, gc->toggle);
	Handle_OP_GroupCancelInvite(&pkt);
}

void TrilogyClient::HandleIncomingGroupDisband(const uint8_t* data, uint32_t len)
{
	LogInfo("[Trilogy][Group] <- OP_GroupDisband (0x4420) raw len={} bytes=[{}]",
	        len, HexDumpBytes(data, len));
	if (!data || len < sizeof(Trilogy::structs::GroupDisband_Struct)) {
		LogInfo("[Trilogy][Group] <- OP_GroupDisband (0x4420) dropped (len={})", len);
		return;
	}
	const auto* wire = reinterpret_cast<const Trilogy::structs::GroupDisband_Struct*>(data);

	// Handle_OP_GroupDisband expects GroupGeneric_Struct { name1, name2 }.
	// v29c only ships ONE name field (the target — self for /disband, the
	// kicked member name for a leader kick); set both EQEmu name slots from
	// it so the kick path (which probes both name1 and name2 via
	// entity_list.GetMob) finds the right Mob.  The wire field is 15B; we
	// NUL-terminate defensively.
	char target_name[16] = {};
	memcpy(target_name, wire->member, sizeof(wire->member));
	target_name[sizeof(target_name) - 1] = '\0';

	EQApplicationPacket pkt(OP_GroupDisband, sizeof(::GroupGeneric_Struct));
	auto* gg = reinterpret_cast<::GroupGeneric_Struct*>(pkt.pBuffer);
	memset(gg, 0, sizeof(::GroupGeneric_Struct));
	strn0cpy(gg->name1, target_name, sizeof(gg->name1));
	strn0cpy(gg->name2, target_name, sizeof(gg->name2));

	LogInfo("[Trilogy][Group] <- OP_GroupDisband (0x4420) target=[{}]", gg->name1);
	Handle_OP_GroupDisband(&pkt);
}

// ============================================================
// SendDoorSpawns — send every door in the current zone to this client as
// EQClassic OP_SpawnDoor (0x9520) packets, one per door.
//
// EQClassic Door_Struct wire layout (46 bytes — full Common struct):
//   [0]   char[16] name        (model/filename, e.g. "DOOR101")
//   [16]  float    yPos
//   [20]  float    xPos
//   [24]  float    zPos
//   [28]  float    heading     (y rotation)
//   [32]  float    incline     (x rotation)
//   [36]  float    padding
//   [40]  uint8    doorid      (zone-local door id, 0-255)
//   [41]  uint8    opentype    (animation style)
//   [42]  uint8    doorIsOpen  (spawn state)
//   [43]  uint8    inverted    (door starts in inverted state)
//   [44]  int16    parameter   (door_param: lift travel distance / button flag)
//
// Why 46 not 44 (previously sent): without parameter, v29c does not register
// a door as a clickable elevator button (FELE2 in gfaydark) and does not know
// how far a FAYLEVATOR platform should travel — clients either silently ignore
// OP_OpenDoor for the platform or animate it indefinitely.  The extra two
// bytes are read from EQClassic Common Door_Struct (`int16 parameter` at
// /*0040*/) which is the layout v29c expects (eq_packet_structs.h:1778).
// ============================================================

void TrilogyClient::SendDoorSpawns()
{
	const auto& doors = entity_list.GetDoorsList();

	int sent = 0;
	for (const auto& kv : doors) {
		Doors* door = kv.second;
		// Match the Titanium MakeDoorSpawnPacket filter: skip placeholder/blank
		// door models (names of 3 chars or fewer are not real renderable doors).
		if (!door || strlen(door->GetDoorName()) <= 3)
			continue;

		const glm::vec4& pos = door->GetPosition();
		const int invert = door->GetInvertState();
		const char* name = door->GetDoorName();

		// Kelethin elevator parts must spawn at rest regardless of any stale
		// server-side m_is_open from prior clicks — a previous test (memory:
		// "Elevator triggered itself on zone-in") showed a non-zero
		// doorIsOpen/inverted combined with a non-zero parameter caused the
		// platform to start moving the moment the player loaded in.  Only
		// elevators get this override; regular doors keep their normal
		// open-at-spawn behavior so traps and pre-opened doors still render
		// correctly.
		const bool is_elevator =
			strncasecmp(name, "FELE",       4)  == 0 ||
			strncasecmp(name, "FAYLEVATOR", 10) == 0;

		uint8_t buf[46];
		memset(buf, 0, sizeof(buf));

		strncpy(reinterpret_cast<char*>(buf), name, 15);
		buf[15] = '\0';
		*reinterpret_cast<float*>(buf + 16) = pos.y;
		*reinterpret_cast<float*>(buf + 20) = pos.x;
		*reinterpret_cast<float*>(buf + 24) = pos.z;
		*reinterpret_cast<float*>(buf + 28) = pos.w; // heading
		*reinterpret_cast<float*>(buf + 32) = static_cast<float>(door->GetIncline());
		// buf+36 padding stays 0
		buf[40] = static_cast<uint8_t>(door->GetDoorID());
		buf[41] = static_cast<uint8_t>(door->GetOpenType());
		if (is_elevator) {
			buf[42] = 0;
			buf[43] = 0;
		} else {
			// Mirror the Titanium state_at_spawn formula: an inverted door reports
			// the negated open state at spawn so its rest position renders correctly.
			bool open_at_spawn = invert ? !door->IsDoorOpen() : door->IsDoorOpen();
			buf[42] = open_at_spawn ? 1 : 0;
			buf[43] = invert ? 1 : 0;
		}
		// int16 parameter at offset 44 — drives v29c's elevator-button detection
		// (FELE2 has door_param=1) and the FAYLEVATOR platform travel distance
		// (door_param=68/98/69 in gfaydark).  For non-elevator doors door_param
		// is typically 0 (no behavior change).
		*reinterpret_cast<int16_t*>(buf + 44) =
			static_cast<int16_t>(door->GetDoorParam());

		if (m_is_zoning) {
			if (m_deferred_spawns.size() < kMaxDeferredSpawns)
				m_deferred_spawns.emplace_back(uint16_t{0x9520},
					std::vector<uint8_t>(buf, buf + sizeof(buf)));
		} else {
			m_tzs->SendToSession(m_session_key, 0x9520, buf, sizeof(buf));
		}
		++sent;
	}

	LogInfo("[TrilogyClient] SendDoorSpawns: {} door(s) {} (46-byte format)", sent,
	        m_is_zoning ? "deferred" : "sent");
}

// ============================================================
// BuildClassicItemFromInst — shared helper used by HandleItemPacket.
//
// Fills a Trilogy::structs::ClassicItem_Struct from a live EQ::ItemInstance*.
// Returns false if inst or its ItemData is null.
// loot_slot is stored in ci.equipslot (pass -1 for cursor, slot ID otherwise).
// ============================================================

static inline int8_t clamp_i8_tc(int32_t v) {
	return static_cast<int8_t>(v < -128 ? -128 : (v > 127 ? 127 : v));
}

// Non-static: trilogy_zone.cpp's PC-trade stage notifier needs this same
// serialiser to build a 292-byte ClassicItem_Struct for OP_TradeItemPacket
// (0xdf20).  Declared in trilogy_zone.cpp via a local extern.
bool BuildClassicItemFromInst(const EQ::ItemInstance* inst,
                              Trilogy::structs::ClassicItem_Struct& ci,
                              int16_t equip_slot)
{
	if (!inst) return false;
	const EQ::ItemData* it = inst->GetItem();
	if (!it) return false;

	memset(&ci, 0, sizeof(ci));

	strncpy(ci.name,   it->Name,   sizeof(ci.name)   - 1);
	// Bow IDFile substitution — see comment at the matching site in
	// trilogy_zone.cpp:SendInventoryItems.  v29c renders the bow during
	// archery animations from this idfile; truncated modern IDs collide
	// with sword models.  "IT4" = v29c bow model (NPC ranger trainers
	// confirmed render bow with melee1 texture=4).
	const char* src_idfile =
	    (it->ItemType == static_cast<uint8_t>(EQ::item::ItemType::ItemTypeBow))
	    ? "IT4"
	    : it->IDFile;
	strncpy(ci.idfile, src_idfile, sizeof(ci.idfile)  - 1);

	// Lore prefix.  EQClassic/Trilogy clients use the first character of the
	// Lore string as a flag marker — confirmed in EQClassic source:
	//   '*' = Lore, '&' = Summoned, '#' = Artifact, '~' = Pending Lore.
	// EQEmu stores these as separate bool fields (LoreFlag / SummonedFlag /
	// ArtifactFlag / PendingLoreFlag) with a clean Lore name, so we have to
	// reconstruct the marker here or the client never shows the "Lore" tag
	// (the NoDrop tag worked because it has a dedicated wire field).
	const char* src_lore = it->Lore;
	if (src_lore && (src_lore[0] == '*' || src_lore[0] == '#' ||
	                 src_lore[0] == '~' || src_lore[0] == '&')) {
		// Legacy data already carries an embedded prefix — skip it so we don't
		// end up with "**name" when the flag is also set in modern columns.
		++src_lore;
	}

	char lore_prefix = 0;
	if      (it->ArtifactFlag)    lore_prefix = '#';
	else if (it->LoreFlag)        lore_prefix = '*';
	else if (it->PendingLoreFlag) lore_prefix = '~';
	else if (it->SummonedFlag)    lore_prefix = '&';

	if (lore_prefix) {
		ci.lore[0] = lore_prefix;
		strncpy(ci.lore + 1, src_lore ? src_lore : "", sizeof(ci.lore) - 2);
	} else {
		strncpy(ci.lore, src_lore ? src_lore : "", sizeof(ci.lore) - 1);
	}

	ci.weight    = static_cast<uint8>(std::min(255, it->Weight > 0 ? it->Weight : 0));
	ci.norent    = static_cast<int8>(it->NoRent);
	ci.nodrop    = static_cast<int8>(it->NoDrop);
	ci.size      = static_cast<uint8>(it->Size);
	ci.itemclass = static_cast<int8>(it->ItemClass);
	if (it->ID > 65535) return false;
	ci.id        = static_cast<uint16>(it->ID);
	ci.icon      = static_cast<uint16>(it->Icon ? it->Icon : 1);
	ci.equipslot   = equip_slot;
	if (it->ItemClass != 1)
		ci.inv_refnum = ci.id;
	ci.slots       = static_cast<uint32>(it->Slots);
	ci.price       = static_cast<int32>(it->Price);

	bool has_click  = (it->Click.Effect  > 0 && it->Click.Effect  < 3000);
	bool has_proc   = (it->Proc.Effect   > 0 && it->Proc.Effect   < 3000);
	bool has_worn   = (it->Worn.Effect   > 0 && it->Worn.Effect   < 3000);
	bool has_scroll = (it->Scroll.Effect > 0 && it->Scroll.Effect < 3000);
	bool has_effect = has_click || has_proc || has_worn || has_scroll;

	if (it->ItemClass == 2) {
		ci.flag = 0x7669;
	} else if (it->ItemClass == 1) {
		ci.flag = (it->BagType > 8) ? 0x3d00 : 0x5450;
	} else {
		ci.flag = has_effect ? 0x0036 : 0x315f;
	}

	if (it->ItemClass == 2) {
		ci.book_data.book     = static_cast<int8>(it->Book);
		ci.book_data.booktype = static_cast<int16>(it->BookType);
		if (it->Filename[0])
			strncpy(ci.book_data.filename, it->Filename, sizeof(ci.book_data.filename) - 1);
	} else if (it->ItemClass == 1) {
		// EQClassic blob loader (database.cpp L1751-1755) zeroes offsets 144-211
		// and 217-285 for containers: "We clean this or the client crashes or bag
		// are full of shit."  Only the header (0-143) and container union (212-216)
		// may be non-zero.  ci is value-initialized (all zero) so we just set the
		// container fields and leave everything else untouched.
		ci.common.container.bagtype   = it->BagType;
		ci.common.container.bagslots  = it->BagSlots > 0 ? it->BagSlots : 1;
		ci.common.container.isbagopen = 0;
		ci.common.container.bagsize   = static_cast<int8>(it->BagSize > 0 ? it->BagSize : 1);
		ci.common.container.bagwr     = it->BagWR;
	} else {
		ci.common.unknown0282 = static_cast<int8>(0xFF);
		ci.common.unknown0283 = static_cast<int8>(0xFF);

		ci.common.astr    = clamp_i8_tc(it->AStr);
		ci.common.asta    = clamp_i8_tc(it->ASta);
		ci.common.acha    = clamp_i8_tc(it->ACha);
		ci.common.adex    = clamp_i8_tc(it->ADex);
		ci.common.aint_   = clamp_i8_tc(it->AInt);
		ci.common.aagi    = clamp_i8_tc(it->AAgi);
		ci.common.awis    = clamp_i8_tc(it->AWis);
		ci.common.mr      = clamp_i8_tc(it->MR);
		ci.common.fr      = clamp_i8_tc(it->FR);
		ci.common.cr      = clamp_i8_tc(it->CR);
		ci.common.dr      = clamp_i8_tc(it->DR);
		ci.common.pr      = clamp_i8_tc(it->PR);
		ci.common.hp      = clamp_i8_tc(it->HP);
		ci.common.mana    = clamp_i8_tc(it->Mana);
		ci.common.ac      = clamp_i8_tc(it->AC);
		ci.common.stackable = it->Stackable ? 1 : 0;
		ci.common.light   = static_cast<uint8>(it->Light);
		ci.common.delay   = static_cast<uint8>(it->Delay);
		ci.common.damage  = static_cast<uint8>(it->Damage > 255 ? 255 : it->Damage);
		ci.common.range_  = static_cast<uint8>(it->Range);
		ci.common.itemtype= static_cast<uint8>(it->ItemType);
		ci.common.magic   = it->Magic ? 1 : 0;
		ci.common.material= static_cast<uint8>(it->Material);
		// items.color for legacy leather/chain/plate gear carries the sentinel
		// 0xFF000000 (alpha=FF, RGB=0). Modern clients ignore that via
		// PP.item_tint.UseTint; v29c stores this value on the item and later
		// echoes it back inside its own OP_WearChange when the player equips
		// the piece, driving its own local render — which multiplies the helm
		// texture by RGB=0 and paints it pitch-black. See NormalizeTintColor.
		ci.common.color   = static_cast<uint32>(Trilogy::NormalizeTintColor(it->Color));
		ci.common.classes = static_cast<uint16>(it->Classes);

		uint16 eff_id    = 0;
		int8   eff_type  = 0;
		int8   eff_level = 0;

		if (has_click) {
			eff_id    = static_cast<uint16>(it->Click.Effect);
			eff_type  = static_cast<int8>(it->Click.Type);
			eff_level = static_cast<int8>(it->Click.Level);
		} else if (has_scroll) {
			eff_id    = static_cast<uint16>(it->Scroll.Effect);
			eff_type  = static_cast<int8>(it->Scroll.Type);
			eff_level = static_cast<int8>(it->Scroll.Level);
		} else if (has_proc) {
			eff_id    = static_cast<uint16>(it->Proc.Effect);
			eff_type  = static_cast<int8>(it->Worn.Type > 0 ? it->Worn.Type : it->Proc.Type);
			eff_level = static_cast<int8>(it->Proc.Level);
		} else if (has_worn) {
			eff_id    = static_cast<uint16>(it->Worn.Effect);
			eff_type  = static_cast<int8>(it->Worn.Type);
			eff_level = static_cast<int8>(it->Worn.Level);
		}

		ci.common.effect1      = eff_id;
		ci.common.effect2      = eff_id;
		ci.common.effecttype1  = eff_type;
		ci.common.effecttype2  = eff_type;
		ci.common.effectlevel1 = static_cast<uint8>(eff_level);
		ci.common.effectlevel2 = static_cast<uint8>(eff_level);

		ci.common.casttime     = static_cast<uint32>(it->CastTime_);
		ci.common.sellrate     = static_cast<float>(it->SellRate);
		ci.common.skillmodtype = static_cast<uint16>(it->SkillModType);
		ci.common.skillmodvalue= static_cast<int16>(it->SkillModValue);
		ci.common.banedmgrace  = static_cast<int16>(it->BaneDmgRace);
		ci.common.banedmgbody  = static_cast<int16>(it->BaneDmgBody);
		ci.common.banedmgamt   = static_cast<uint8>(it->BaneDmgAmt > 255 ? 255 : it->BaneDmgAmt);
		ci.common.reclevel     = static_cast<uint8>(it->RecLevel);
		ci.common.recskill     = static_cast<uint8>(it->RecSkill);
		ci.common.procrate     = static_cast<uint16>(it->ProcRate < 0 ? 0 : it->ProcRate);
		ci.common.elemdmgtype  = static_cast<uint8>(it->ElemDmgType);
		ci.common.elemdmgamt   = static_cast<uint8>(it->ElemDmgAmt);
		ci.common.factionmod1  = static_cast<uint16>(it->FactionMod1 < 0 ? 0 : it->FactionMod1);
		ci.common.factionmod2  = static_cast<uint16>(it->FactionMod2 < 0 ? 0 : it->FactionMod2);
		ci.common.factionmod3  = static_cast<uint16>(it->FactionMod3 < 0 ? 0 : it->FactionMod3);
		ci.common.factionmod4  = static_cast<uint16>(it->FactionMod4 < 0 ? 0 : it->FactionMod4);
		ci.common.factionamt1  = static_cast<uint16>(it->FactionAmt1 < 0 ? 0 : it->FactionAmt1);
		ci.common.factionamt2  = static_cast<uint16>(it->FactionAmt2 < 0 ? 0 : it->FactionAmt2);
		ci.common.factionamt3  = static_cast<uint16>(it->FactionAmt3 < 0 ? 0 : it->FactionAmt3);
		ci.common.factionamt4  = static_cast<uint16>(it->FactionAmt4 < 0 ? 0 : it->FactionAmt4);
		ci.common.deity        = static_cast<uint16>(it->Deity);

		ci.common.normal.races = static_cast<uint16>(it->Races);
		if (has_click)
			ci.common.normal.click_effect_type = (it->Click.Type == 5) ? 3 : static_cast<int8>(it->Click.Type);
		else if (has_worn)
			ci.common.normal.click_effect_type = static_cast<int8>(it->Worn.Type);
		else if (has_scroll)
			ci.common.normal.click_effect_type = static_cast<int8>(it->Scroll.Type);
		else if (has_proc)
			ci.common.normal.click_effect_type = 2;

		int16_t ch = inst->GetCharges();
		ci.common.charges = (ch == 0) ? static_cast<int8>(-1) :
		                    static_cast<int8>(ch > 127 ? 127 : ch);
	}

	return true;
}

// ============================================================
// HandleItemPacket — OP_ItemPacket (server → Trilogy client).
//
// The server sends OP_ItemPacket with a serialised item for loot window
// display (ItemPacketLoot), cursor delivery (ItemPacketLimbo), or
// inventory display (ItemPacketCharInventory).
//
// The packet is: { ItemPacketType PacketType (4 bytes) } followed by
// the binary bytes of InternalSerializedItem_Struct { int16 slot_id;
// const void* inst; } — inst is a live pointer to EQ::ItemInstance.
//
// We extract the pointer, build a ClassicItem_Struct, and send it:
//   ItemPacketLoot      → 0x5220 (OP_ItemOnCorpse), slot_id = loot slot
//   ItemPacketLimbo     → itemclass opcode (6421/6521/6621), slot 0 (cursor)
//   ItemPacketCharInventory → itemclass opcode, translated slot
// ============================================================

void TrilogyClient::HandleItemPacket(const EQApplicationPacket* app)
{
	if (!app) return;

	// Minimum size: ItemPacketType (4) + InternalSerializedItem_Struct (>=10 with pointer)
	if (app->size < 4 + sizeof(EQ::InternalSerializedItem_Struct))
		return;

	const auto pkt_type = static_cast<ItemPacketType>(
	    *reinterpret_cast<const int32_t*>(app->pBuffer));

	const auto* isi = reinterpret_cast<const EQ::InternalSerializedItem_Struct*>(
	    app->pBuffer + 4);

	const auto* inst = reinterpret_cast<const EQ::ItemInstance*>(isi->inst);
	if (!inst) return;

	int16_t slot_id = isi->slot_id;

	// Determine output equip_slot for the ClassicItem_Struct.
	// Trilogy loot slots pass through; cursor (limbo) is slot 0 in v29c.
	int16_t equip_slot;
	uint16_t wire_opcode;

	switch (pkt_type) {
	case ItemPacketLoot: {
		// EQEmu's Corpse::MakeLootRequestPackets iterates loot_slot from CORPSE_BEGIN
		// (23) skipping bits absent from CORPSE_BITMASK, so emu slots arrive sparse
		// (e.g. 23-30, 33, 34-54, 56) — that's what the `<< 34` mask layout produces
		// to avoid the PossessionsBitmask conflict at slots 31/32.  A raw slot_id-22
		// translation would give the v29c client wire slots 1-8, 11, 12-32, 34 with
		// gaps and a lone slot 34 that overflows the client's 30-entry corpse array.
		//
		// Renumber to sequential 1..30 to match EQClassic's `counter++` pattern
		// (Zone/Source/PlayerCorpse.cpp:626-653) — the wire → emu mapping is stored
		// on TrilogyClient so ZN_OP_LootItem's inbound handler can recover the
		// correct emu slot for Handle_OP_LootItem.  Reset per LootRequest.
		const int16_t wire = AssignLootWireSlot(slot_id);
		if (wire == 0) return; // over-cap; dropped by AssignLootWireSlot
		equip_slot   = wire;
		wire_opcode  = 0x5220; // OP_ItemOnCorpse
		LogInfo("[TRILOGY-LOOT] HandleItemPacket ItemPacketLoot: emu_slot={} → wire_slot={} item_id={}",
		        slot_id, equip_slot,
		        inst->GetItem() ? inst->GetItem()->ID : 0);
		break;
	}
	case ItemPacketLimbo:
		// Cursor delivery when cursor was already occupied (pre-RoF path in PutLootInInventory).
		// Treat identically to ItemPacketTrade at slotCursor: send OP_SummonedItem (0x7821)
		// so the EQClassic client receives the item on cursor.
		equip_slot  = 0;
		wire_opcode = 0x7821; // OP_SummonedItem — cursor delivery
		break;
	case ItemPacketTrade:
		// Looted item delivery.  Same slot translation as ItemPacketCharInventory and
		// SendInventoryItems: EQEmu slots 0-21 (equipment) share the same numbering as
		// EQClassic, while slot 22+ (ammo, general, bags) are shifted down by 1 because
		// EQClassic has no charm slot (EQEmu slot 0) and no power-source slot (EQEmu slot 21).
		// Using slot_id-1 for equipment slots would put an item in the wrong worn slot
		// (e.g. slotPrimary=13 → 12=hands) and overwrite whatever is displayed there.
		// • slotCursor (33): cursor delivery via OP_SummonedItem (0x7821), equip_slot=0.
		// • cursor bag content (EQEmu 351-360): OP_ItemTradeIn (0x3120) at wire 330-339
		//   — EQClassic's PutItemInInventory (client.cpp:2235-2273) writes
		//   pp.cursorbaginventory[slotid-330] for wire slots 330-339, but EQEmu
		//   internally addresses cursor bag content at invbag::CURSOR_BAG_BEGIN (351).
		//   Without this remap the packet lands in an unmapped wire range and the
		//   client renders the bag as empty — the reported "bag looted but arrives
		//   empty" symptom for right-click loot of bags that landed on cursor.
		// • general bag content (EQEmu 251-330): OP_ItemTradeIn (0x3120) at wire
		//   250-329 (slot_id-1 shift); wire formula per EQClassic is
		//   containerinv[slotid-250], and 250+(bag_wire-22)*10+i matches the
		//   general slot 23-30 → wire 22-29 shift already applied to the parent bag.
		// • all other slots: OP_ItemTradeIn (0x3120) with the correct EQClassic equip_slot.
		// Bank slots (DB 2000-2110) are owned by SendInventoryItems (EQClassic-faithful
		// 0x3120 pass) — drop here so the engine's m_inv post-zone-in pump doesn't
		// double-deliver bank top items.
		if (slot_id >= 2000 && slot_id <= 2110) return;
		if (slot_id == EQ::invslot::slotCursor) {
			equip_slot  = 0;
			wire_opcode = 0x7821; // OP_SummonedItem — cursor delivery
		} else if (slot_id >= EQ::invbag::CURSOR_BAG_BEGIN &&
		           slot_id <= EQ::invbag::CURSOR_BAG_END) {
			// Server 351..360 → wire 330..339 (matches EQClassic client's
			// pp.cursorbaginventory[slotid-330] parse).
			equip_slot  = static_cast<int16_t>(slot_id - 21);
			wire_opcode = 0x3120;
		} else {
			equip_slot  = (slot_id >= 22) ? static_cast<int16_t>(slot_id - 1)
			                              : static_cast<int16_t>(slot_id);
			wire_opcode = 0x3120;
		}
		LogInfo("[TRILOGY-LOOT] HandleItemPacket ItemPacketTrade: emu_slot={} wire_slot={} wire_op=0x{:04x} item_id={}",
		        slot_id, equip_slot, wire_opcode,
		        inst->GetItem() ? inst->GetItem()->ID : 0);
		break;
	case ItemPacketCharInventory:
		// General inventory delivery: translate EQEmu slot to Trilogy slot.
		// Slots 0-21 pass through; 22-30 → 21-29 (no charm slot in v29c).
		// Bank slots owned by SendInventoryItems — see ItemPacketTrade comment.
		if (slot_id >= 2000 && slot_id <= 2110) return;
		equip_slot  = (slot_id >= 22) ? static_cast<int16_t>(slot_id - 1) : slot_id;
		wire_opcode = (inst->GetItem() && inst->GetItem()->ItemClass == 1) ? 0x6621 :
		              (inst->GetItem() && inst->GetItem()->ItemClass == 2) ? 0x6521 : 0x6421;
		break;
	case ItemPacketWorldContainer:
		// Tradeskill world container slot delivery, fired from Object::HandleClick
		// for each occupied bag slot after the OP_CraftingStation open.  EQEmu's
		// slot_id is the bag interior index 0..9 (EQ::invbag::SLOT_BEGIN..SLOT_END);
		// EQClassic's SendStationItem writes item.equipSlot = i (0..9) on wire
		// OP_StationItem 0xfb20.  The 4000-4009 wire range only shows up in
		// OP_MoveItem traffic (handled in TrilogyZoneServer::HandleMoveItem).
		equip_slot  = slot_id;
		wire_opcode = 0xfb20; // OP_StationItem
		LogInfo("[TRILOGY-TS] HandleItemPacket ItemPacketWorldContainer: bag_idx={} item_id={}",
		        slot_id, inst->GetItem() ? inst->GetItem()->ID : 0);
		break;
	case ItemPacketMerchant: {
		// Merchant window item.  EQEmu's BulkSendMerchantInventory sends
		// SendItemPacket(ml.slot - 1, inst, ItemPacketMerchant), so slot_id is the
		// 0-based window slot the client echoes back on buy.  The full buy price is
		// already baked into inst->GetPrice(); we divide by the price multiplier so
		// the client's redisplay (cost * pricemultiplier) shows the real buy price.
		//
		// ── v29c merchant window cap (30 items, hard) ────────────────────────
		// The client allocates a fixed `uint16 merchantgoods[30]` (EQClassic
		// client.h:459).  Their server guards both initial fill (client.cpp:1255
		// `<30`) and post-sell adds (client.cpp:1395 `if (totalitemdisplayed
		// == 30) return;`) — over-cap items are silently dropped.  Sending
		// OP_ShopItem with slot_id >= 30 walks past the array; corruption is
		// latent and surfaces a few transactions later as an AV reading a
		// pointer overwritten with item-name bytes (faulting addr decodes as
		// ASCII string fragment).  Drop here to keep the wire and the client's
		// buffer in sync.  Side effect: sells beyond the cap still complete
		// server-side (player gets money, item is added to merchant temp DB) —
		// the player just can't see it in the open window, matching EQClassic.
		static constexpr int kTrilogyMerchantWindowCap = 30;
		if (slot_id >= kTrilogyMerchantWindowCap) {
			LogInfo("[TRILOGY-MERCHANT] dropping ItemPacketMerchant slot={} (>= v29c cap {}) item={}",
			        slot_id, kTrilogyMerchantWindowCap,
			        inst->GetItem() ? inst->GetItem()->ID : 0);
			return;
		}

		Trilogy::structs::ClassicItem_Struct mci{};
		if (!BuildClassicItemFromInst(inst, mci, slot_id))
			return;

		const float rate = (m_merchant_rate > 0.0001f) ? m_merchant_rate : 1.0f;
		int64_t shown = static_cast<int64_t>(std::lround(static_cast<double>(inst->GetPrice()) / rate));
		if (shown < 0) shown = 0;
		mci.price = static_cast<int32_t>(shown > INT32_MAX ? INT32_MAX : shown);

		const EQ::ItemData* mit = inst->GetItem();

		// Finite player-sold/unique stock: surface the stocked quantity in the
		// charges field so the client shows (and lets you buy back) more than one.
		// The Trilogy ClassicItem has no separate merchant-quantity field; charges
		// is the only place the client reads a count from.  (Regular stock has
		// merchant_count == -1 and keeps BuildClassicItemFromInst's charges value.)
		const int32_t mcount = inst->GetMerchantCount();
		if (mit && mit->ItemClass == 0 /* common (has .common union) */ && mcount > 0) {
			mci.common.charges = static_cast<int8_t>(mcount > 127 ? 127 : mcount);
		}

		// Record what the client now sees at this slot for the buy/sell handlers.
		MerchantWindowEntry e{};
		e.item_id        = mit ? mit->ID : 0;
		e.price          = inst->GetPrice();   // charged as-is (per unit)
		e.merchant_count = inst->GetMerchantCount(); // -1 = infinite regular stock
		e.merchant_slot  = inst->GetMerchantSlot();
		e.charges        = inst->GetCharges();
		SetMerchantWindowItem(slot_id, e);

		// Merchant LIST id (NPC MerchantType) for the Item_Shop_Struct header.
		uint32_t merchant_list_id = 0;
		if (Mob* mm = entity_list.GetMob(m_merchant_npc_id))
			if (mm->IsNPC())
				merchant_list_id = mm->CastToNPC()->MerchantType;

		Trilogy::structs::Item_Shop_Struct iss{};
		memset(&iss, 0, sizeof(iss));
		iss.merchantid = merchant_list_id;
		iss.itemtype   = mci.itemclass;
		iss.item       = mci;

		m_tzs->SendToSession(m_session_key, 0x0c20,
		                     reinterpret_cast<const uint8_t*>(&iss),
		                     static_cast<uint32_t>(sizeof(iss)));
		return;
	}
	default:
		return; // Other item packet types not yet translated.
	}

	Trilogy::structs::ClassicItem_Struct ci{};
	if (!BuildClassicItemFromInst(inst, ci, equip_slot))
		return;

	if (pkt_type == ItemPacketLoot || pkt_type == ItemPacketTrade || pkt_type == ItemPacketLimbo) {
		LogInfo("[TRILOGY-LOOT] HandleItemPacket WIRE op=0x{:04x} wire_slot={} item_id={} item_class={} bagtype={} charges={} stack={}",
		        wire_opcode, equip_slot,
		        inst->GetItem() ? inst->GetItem()->ID : 0,
		        inst->GetItem() ? (int)inst->GetItem()->ItemClass : -1,
		        inst->GetItem() ? (int)inst->GetItem()->BagType : -1,
		        inst->GetCharges(),
		        inst->IsStackable() ? 1 : 0);

		// Hex-dump the full 292-byte ClassicItem_Struct so we can diff a working
		// item against a v29c-killing item. Loot crashes are content-dependent —
		// the slot/id/order all check out but some field inside the struct chokes
		// the client. Without the bytes we are guessing.
		const auto* raw = reinterpret_cast<const uint8_t*>(&ci);
		const size_t total = sizeof(ci);
		const size_t row   = 32;
		const char*  iname = (inst->GetItem() && inst->GetItem()->Name) ? inst->GetItem()->Name : "?";
		LogInfo("[TRILOGY-LOOT] HEX DUMP {} bytes for item_id={} name=[{}]",
		        static_cast<unsigned>(total),
		        inst->GetItem() ? inst->GetItem()->ID : 0,
		        iname);
		for (size_t off = 0; off < total; off += row) {
			std::string line = fmt::format("[TRILOGY-LOOT] @0x{:03x}:", off);
			const size_t end = std::min(off + row, total);
			for (size_t i = off; i < end; ++i)
				line += fmt::format(" {:02x}", static_cast<unsigned>(raw[i]));
			LogInfo("{}", line);
		}
	}

	// Cursor deliveries route through the deferred-summon queue so multi-item
	// loot doesn't silently drop past the first item (v29c only renders one
	// cursor item at a time).  ItemPacketLimbo, ItemPacketTrade@slotCursor, and
	// (via ItemPacketLoot) OP_SummonedItem all share opcode 0x7821 wire-side.
	if (wire_opcode == 0x7821) {
		EnqueueOrSendSummonedItem(reinterpret_cast<const uint8_t*>(&ci),
		                          static_cast<uint32_t>(sizeof(ci)));
	} else {
		m_tzs->SendToSession(m_session_key, wire_opcode,
		                     reinterpret_cast<const uint8_t*>(&ci),
		                     static_cast<uint32_t>(sizeof(ci)));
	}

	// Loot echo (0xa020) is flushed by the caller in trilogy_zone.cpp
	// AFTER Handle_OP_LootItem returns — matching EQClassic order where
	// the echo follows ALL item deliveries (bag + contents), not just the
	// first item.  Flushing here would place the echo between the bag and
	// its content items, which crashes the v29c client.
	if (pkt_type == ItemPacketLimbo) {
		LogInfo("[TRILOGY-LOOT] HandleItemPacket ItemPacketLimbo: flushing echo NOW (cursor path)");
		FlushPendingLootEcho();
	}
}
