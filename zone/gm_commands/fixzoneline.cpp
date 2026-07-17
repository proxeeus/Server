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

// #fixzoneline — capture the GM's current position into a broken
// trilogy_zone_points row's source coordinates. Covers the gap left by
// EQClassic's #createzoneline (which recorded source coords on the ONE side
// its GM ran the command from, leaving the reverse-direction row's source at
// placeholder (0,0,0) forever).
//
// Two forms:
//   #fixzoneline <id>            update a specific row (unambiguous — for zones
//                                with multiple broken rows to the same target
//                                like droga↔nurga (2 tunnels), skyshrine's
//                                59 self-teleporters, etc.)
//
//   #fixzoneline <target_zone>   shortcut for the common single-exit case
//                                (velketor→greatdivide etc.). Only applies if
//                                exactly ONE broken row matches
//                                (current_zone, target_zone); if 0 or 2+ match,
//                                refuses and prints the candidate list so the
//                                GM re-runs with an explicit id.
//
// The row's source coords become the GM's current (x, y, z). Zrange is bumped
// to 15 to accommodate the natural offset between "where the GM stood clicking
// the command" and "where players actually walk into the door" (typically a
// few units of variance depending on which side of the door face they're on).
//
// Both the DB row and the in-memory zone->trilogy_zone_line_list entry are
// updated in-place so the fix takes effect immediately for any Trilogy player
// zoning through — no #reload static required.

void command_fixzoneline(Client *c, const Seperator *sep)
{
	if (!c || !zone) {
		return;
	}

	if (sep->argnum < 1 || !sep->arg[1] || !*sep->arg[1]) {
		c->Message(Chat::White, "Usage: #fixzoneline <id>  OR  #fixzoneline <target_zone_shortname>");
		c->Message(Chat::White, "  Run #zonelines first to see this zone's broken rows.");
		return;
	}

	if (zone->trilogy_zone_line_list.empty()) {
		c->Message(
			Chat::White,
			fmt::format(
				"[#fixzoneline] Zone [{}] has NO trilogy_zone_points rows loaded — nothing to fix.",
				zone->GetShortName()
			).c_str()
		);
		return;
	}

	// Argument parsing: numeric = row id, alphanumeric = target zone shortname.
	// Strings::IsNumber accepts leading '-' which is fine (row ids are always
	// positive so a negative parse just fails to find a row).
	const std::string arg1 = sep->arg[1];
	int32 target_id = 0;
	bool  arg_is_id = Strings::IsNumber(arg1);
	if (arg_is_id) {
		target_id = Strings::ToInt(arg1);
	}

	// Resolve the target row. Two paths depending on the argument shape.
	TrilogyZoneLineNode* target_row = nullptr;

	if (arg_is_id) {
		// Direct id lookup. Row must exist in the CURRENT zone's list
		// (belongs to this zone) to prevent accidentally touching another
		// zone's data from here.
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
					"[#fixzoneline] No row with id={} in zone [{}]'s trilogy_zone_points."
					" Run #zonelines to see the valid ids for this zone.",
					target_id, zone->GetShortName()
				).c_str()
			);
			return;
		}
	}
	else {
		// Shortname shortcut — find the unique broken row matching this
		// (zone, target_zone) pair. Ambiguity is an error, not a guess.
		const uint32 target_zone_id = ZoneID(arg1.c_str());
		if (target_zone_id == 0) {
			c->Message(
				Chat::White,
				fmt::format(
					"[#fixzoneline] '{}' is not a valid zone shortname.",
					arg1.c_str()
				).c_str()
			);
			return;
		}

		std::vector<TrilogyZoneLineNode*> matches;
		for (auto &zln : zone->trilogy_zone_line_list) {
			const bool is_broken = (zln.x == 0.0f && zln.y == 0.0f && zln.z == 0.0f);
			if (is_broken && zln.target_zone_id == static_cast<uint16>(target_zone_id)) {
				matches.push_back(&zln);
			}
		}

		if (matches.empty()) {
			c->Message(
				Chat::White,
				fmt::format(
					"[#fixzoneline] No BROKEN row in zone [{}] targets [{}]."
					" Either the transition is already fixed or no such"
					" trilogy_zone_points entry exists.",
					zone->GetShortName(), arg1.c_str()
				).c_str()
			);
			return;
		}
		if (matches.size() > 1) {
			c->Message(
				Chat::Yellow,
				fmt::format(
					"[#fixzoneline] Ambiguous: {} broken rows in [{}] target [{}]."
					" Re-run with an explicit id from the list below:",
					(unsigned) matches.size(), zone->GetShortName(), arg1.c_str()
				).c_str()
			);
			for (auto* m : matches) {
				c->Message(
					Chat::White,
					fmt::format(
						"  id={} : lands at ({:.1f}, {:.1f}, {:.1f})  ->  #fixzoneline {}",
						m->id, m->target_x, m->target_y, m->target_z, m->id
					).c_str()
				);
			}
			return;
		}
		target_row = matches[0];
	}

	// Overwrite protection was here originally, but disambiguating multi-door
	// zones (droga↔nurga, chardok↔burningwood, and every other pair where
	// both sides have (0,0,0) sources) can only be done by empirical swap-
	// and-test. If we refused overwrites, the GM couldn't try swapping
	// row 1212 with row 1213 to see which pairing feels right. So we just
	// warn when overwriting a non-placeholder row and proceed anyway.
	const bool row_was_broken = (target_row->x == 0.0f && target_row->y == 0.0f && target_row->z == 0.0f);
	if (!row_was_broken) {
		c->Message(
			Chat::Yellow,
			fmt::format(
				"[#fixzoneline] Row id={} already had source coords"
				" ({:.1f}, {:.1f}, {:.1f}) - OVERWRITING with your current position.",
				target_row->id, target_row->x, target_row->y, target_row->z
			).c_str()
		);
	}

	// Capture GM position, snap Zrange up to 15 for the "door area" tolerance.
	const float new_x      = c->GetX();
	const float new_y      = c->GetY();
	const float new_z      = c->GetZ();
	constexpr int new_zrange = 15;

	// Persist to DB first. If the write fails, don't touch the in-memory list.
	const std::string q = fmt::format(
		"UPDATE trilogy_zone_points"
		" SET x = {:.2f}, y = {:.2f}, z = {:.2f}, Zrange = {}"
		" WHERE id = {}",
		new_x, new_y, new_z, new_zrange, target_row->id
	);
	auto results = content_db.QueryDatabase(q);
	if (!results.Success()) {
		c->Message(
			Chat::Red,
			fmt::format(
				"[#fixzoneline] DB UPDATE failed for id={}: {}",
				target_row->id, results.ErrorMessage()
			).c_str()
		);
		return;
	}

	// Update the in-memory row so the fix is live for any Trilogy player in
	// this zone right now — no #reload static needed.
	target_row->x      = new_x;
	target_row->y      = new_y;
	target_row->z      = new_z;
	target_row->Zrange = new_zrange;

	const std::string dest_zone_short = ZoneName(target_row->target_zone_id, true);

	c->Message(
		Chat::Green,
		fmt::format(
			"[#fixzoneline] Fixed row id={} in zone [{}] -> [{}] : source now"
			" ({:.2f}, {:.2f}, {:.2f}), Zrange={}. Row is live in-memory;"
			" Trilogy players walking to this door will now zone.",
			target_row->id,
			zone->GetShortName(),
			dest_zone_short.c_str(),
			new_x, new_y, new_z, new_zrange
		).c_str()
	);

	LogInfo(
		"[TrilogyZP] #fixzoneline applied by {} (charid={}): row id={}"
		" zone=[{}] -> [{}] source=({:.2f},{:.2f},{:.2f}) Zrange={}",
		c->GetCleanName(), c->CharacterID(),
		target_row->id,
		zone->GetShortName(), dest_zone_short.c_str(),
		new_x, new_y, new_z, new_zrange
	);
}
