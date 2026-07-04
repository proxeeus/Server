/*	EQEMu: Everquest Server Emulator
	Copyright (C) 2001-2016 EQEMu Development Team (http://eqemulator.org)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE. See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "../client.h"
#include "../../common/repositories/trilogy_zone_points_repository.h"

// #fixzonerange <id> <new_range>
//
// Adjusts the Zrange (box half-side) of a trilogy_zone_points row and applies
// the change live in memory so the new detection radius is effective for any
// Trilogy player in the zone right now — no #reload static or restart.
//
// Zrange semantics from CheckTrilogyZoneLines (UseNewZoning=0 old-mode box):
//   fires when |player.x - row.x| <= Zrange AND |player.y - row.y| <= Zrange
//
// So Zrange = 15 gives a 30x30 detection box, Zrange = 50 gives 100x100, etc.
//
// Typical values by zoneline type (empirical from EQClassic imports):
//   - dungeon doors:    5-15u  (tight, precise door face)
//   - city gates:      15-30u  (wider, easy to walk through)
//   - outdoor edges:   50-150u (huge seamless boundaries like ecommons/nro)
//   - teleporter pads:  3-5u   (step-on tightness)
//
// Two argument forms:
//   #fixzonerange <id> <range>       explicit — direct row id + new range
//   #fixzonerange <range>            auto-pick — applies to the row in the
//                                    current zone whose source coord is
//                                    closest to your position (i.e. the door
//                                    you're standing at). Useful when you
//                                    already fixed the position and want to
//                                    dial in the range without looking up
//                                    the id again.
//
// Both forms accept any positive integer. The safety guard is a warning at
// range > 500 (very wide) but the update proceeds anyway — some outdoor lines
// legitimately need huge boxes.

void command_fixzonerange(Client *c, const Seperator *sep)
{
	if (!c) {
		return;
	}

	if (sep->argnum < 1 || !sep->arg[1] || !*sep->arg[1]) {
		c->Message(Chat::White, "Usage: #fixzonerange <id> <new_range>       - explicit row id + range");
		c->Message(Chat::White, "       #fixzonerange <new_range>            - auto-pick nearest row in current zone");
		c->Message(Chat::White, "  Range is the box half-side in units. Effective box is 2*range on each axis.");
		c->Message(Chat::White, "  Typical values: 5-15 dungeon doors, 15-30 city gates, 50-150 outdoor seamless.");
		return;
	}

	// Parse forms:
	//   1 arg  = range only (auto-pick row by proximity)
	//   2 args = id + range (explicit)
	int32 target_id  = 0;
	int32 new_range  = 0;
	bool  auto_pick  = false;

	if (sep->argnum >= 2 && sep->arg[2] && *sep->arg[2]) {
		// Two args: id + range
		if (!Strings::IsNumber(sep->arg[1]) || !Strings::IsNumber(sep->arg[2])) {
			c->Message(Chat::White, "Both arguments must be numeric. Usage: #fixzonerange <id> <new_range>");
			return;
		}
		target_id = Strings::ToInt(sep->arg[1]);
		new_range = Strings::ToInt(sep->arg[2]);
	}
	else {
		// One arg: just the range. Auto-pick by proximity.
		if (!Strings::IsNumber(sep->arg[1])) {
			c->Message(Chat::White, "Argument must be numeric. Usage: #fixzonerange <new_range>");
			return;
		}
		new_range = Strings::ToInt(sep->arg[1]);
		auto_pick = true;
	}

	if (new_range <= 0) {
		c->Message(Chat::White, "[#fixzonerange] Range must be a positive integer.");
		return;
	}
	if (new_range > 500) {
		c->Message(
			Chat::Yellow,
			fmt::format(
				"[#fixzonerange] WARNING: {} is very large (~{}u effective box). "
				"Proceeding anyway - some outdoor seamless boundaries legitimately"
				" need this, but double-check the value.",
				new_range, new_range * 2
			).c_str()
		);
	}

	// Resolve the target row.
	TrilogyZoneLineNode* target_row = nullptr;

	if (auto_pick) {
		// Auto-pick: closest row in current zone whose SOURCE coord is near
		// the GM's position. That's the row associated with the door the GM
		// is standing at. Broken (0,0,0) source rows are excluded because
		// proximity to them from anywhere except origin is meaningless.
		if (!zone) {
			c->Message(Chat::White, "[#fixzonerange] No current zone context - auto-pick needs to be in a zone.");
			return;
		}
		float best_dist2 = -1.0f;
		for (auto &zln : zone->trilogy_zone_line_list) {
			if (zln.x == 0.0f && zln.y == 0.0f && zln.z == 0.0f) {
				continue; // skip placeholder rows
			}
			const float dx = zln.x - c->GetX();
			const float dy = zln.y - c->GetY();
			const float d2 = dx * dx + dy * dy;
			if (best_dist2 < 0.0f || d2 < best_dist2) {
				best_dist2 = d2;
				target_row = &zln;
			}
		}
		if (!target_row) {
			c->Message(
				Chat::White,
				fmt::format(
					"[#fixzonerange] No usable rows found in zone [{}] (all sources are 0,0,0)."
					" Use explicit form: #fixzonerange <id> <range>",
					zone->GetShortName()
				).c_str()
			);
			return;
		}
		const float dist_2d = std::sqrt(best_dist2);
		c->Message(
			Chat::White,
			fmt::format(
				"[#fixzonerange] Auto-picked row id={} : source ({:.1f},{:.1f},{:.1f})"
				" -> {} ({}), source is {:.0f}u from your position.",
				target_row->id,
				target_row->x, target_row->y, target_row->z,
				ZoneName(target_row->target_zone_id, true),
				target_row->target_zone_id,
				dist_2d
			).c_str()
		);
		if (dist_2d > 100.0f) {
			c->Message(
				Chat::Yellow,
				fmt::format(
					"[#fixzonerange] WARNING: pick is {:.0f}u from your position - probably not the row you meant."
					" Walk closer to the specific door and rerun, or use explicit form.",
					dist_2d
				).c_str()
			);
		}
		target_id = target_row->id;
	}
	else {
		// Explicit id form. Row must belong to the CURRENT zone (safety —
		// live in-memory update only makes sense here; changing another
		// zone's Zrange via this command would leave that zone's in-memory
		// copy stale until #reload static).
		if (!zone) {
			c->Message(Chat::White, "[#fixzonerange] No current zone context.");
			return;
		}
		for (auto &zln : zone->trilogy_zone_line_list) {
			if (zln.id == target_id) {
				target_row = &zln;
				break;
			}
		}
		if (!target_row) {
			c->Message(
				Chat::White,
				fmt::format(
					"[#fixzonerange] No row with id={} in zone [{}]'s trilogy_zone_points."
					" Range changes to rows in OTHER zones need to be made via SQL"
					" and picked up on that zone's next #reload static.",
					target_id, zone->GetShortName()
				).c_str()
			);
			return;
		}
	}

	const int32 old_range = target_row->Zrange;

	// Persist to DB first. If the write fails, don't touch in-memory.
	const std::string update_q = fmt::format(
		"UPDATE trilogy_zone_points SET Zrange = {} WHERE id = {}",
		new_range, target_id
	);
	auto results = content_db.QueryDatabase(update_q);
	if (!results.Success()) {
		c->Message(
			Chat::Red,
			fmt::format(
				"[#fixzonerange] DB UPDATE failed for id={}: {}",
				target_id, results.ErrorMessage()
			).c_str()
		);
		return;
	}

	// Live in-memory update — effective on next position tick for any Trilogy
	// player already in this zone. No #reload static required.
	target_row->Zrange = new_range;

	c->Message(
		Chat::Green,
		fmt::format(
			"[#fixzonerange] Row id={} Zrange updated: {} -> {} (effective detection"
			" box is now {}x{}u). Live in memory - test by walking into the door.",
			target_id, old_range, new_range, new_range * 2, new_range * 2
		).c_str()
	);

	LogInfo(
		"[TrilogyZP] #fixzonerange applied by {} (charid={}): row id={}"
		" zone=[{}] -> [{}] Zrange {} -> {}",
		c->GetCleanName(), c->CharacterID(),
		target_id,
		target_row->id,
		ZoneName(target_row->target_zone_id, true),
		old_range, new_range
	);
}
