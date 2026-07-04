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

// #zonelines — list every trilogy_zone_points row loaded for the current zone,
// highlighting rows with source coords (0,0,0) — the "broken/placeholder" rows
// for which server-side detection cannot fire until the actual door position
// is captured in this zone.
//
// For each broken row, if a reverse-direction row exists (target_zone points
// back to the current zone with real source coords), we ALSO look up the
// reverse row's TARGET — that's approximately where players LAND in the
// current zone when arriving from the destination, which is a good hint at
// where the actual door is on THIS side. A #goto saylink is emitted so the
// GM can teleport there instantly, walk the last few units to line up on
// the actual door face, and #fixzoneline from that spot.
//
// Companion to #fixzoneline. Typical workflow:
//   1. #zonelines                       (see broken rows + door-hint saylinks)
//   2. Click the [Goto door] saylink    (teleport to the reverse-target hint)
//   3. Walk to the actual door face     (a few units of alignment)
//   4. #fixzoneline <id>                (write GM position into the row)
//   5. Repeat for each broken exit
//
// Read-only. Emits one row per line with the row id, target zone shortname,
// and the "landing coord" in the destination — the target coord authored on
// this row is where players arrive on the OTHER side, which is a hint at
// which door in this zone maps to which broken row (the destination Y/Z tell
// you if it's the north or south tunnel, upper or lower floor, etc.).
void command_zonelines(Client *c, const Seperator *sep)
{
	if (!c || !zone) {
		return;
	}

	if (zone->trilogy_zone_line_list.empty()) {
		c->Message(
			Chat::White,
			fmt::format(
				"[#zonelines] Zone [{}] has NO trilogy_zone_points rows loaded."
				" All zoning here uses the modern EQEmu zone_points table via"
				" the sphere fallback path.",
				zone->GetShortName()
			).c_str()
		);
		return;
	}

	int broken_count = 0;
	int usable_count = 0;

	c->Message(
		Chat::White,
		fmt::format(
			"[#zonelines] trilogy_zone_points inventory for zone [{}] ({} row(s) loaded):",
			zone->GetShortName(),
			(unsigned) zone->trilogy_zone_line_list.size()
		).c_str()
	);
	c->SendChatLineBreak();

	for (const auto &zln : zone->trilogy_zone_line_list) {
		const bool is_broken = (zln.x == 0.0f && zln.y == 0.0f && zln.z == 0.0f);
		if (is_broken) {
			++broken_count;
		}
		else {
			++usable_count;
		}

		const std::string dest_zone_short = ZoneName(zln.target_zone_id, true);

		if (is_broken) {
			// Reverse-pair lookup for door-location hint.
			//
			// Query: find a row in the DESTINATION zone that points BACK to
			// the current zone with real source coords. Its TARGET is where
			// players LAND in the current zone when arriving from that
			// direction — which is right next to the door on this side.
			//
			// If found, emit a #goto saylink so the GM can teleport straight
			// to the door area. If not, tell the GM they'll need to find the
			// door on foot (rare — only when the reverse-side row is also
			// broken or missing).
			auto reverse_rows = TrilogyZonePointsRepository::GetWhere(
				content_db,
				fmt::format(
					"zone = '{}' AND target_zone = '{}'"
					" AND (x != 0 OR y != 0 OR z != 0)"
					" AND id != {}"
					" LIMIT 1",
					Strings::Escape(dest_zone_short),
					Strings::Escape(zone->GetShortName()),
					zln.id
				)
			);

			c->Message(
				Chat::Red,
				fmt::format(
					"[BROKEN id={}] -> {} ({}) | lands players at ({:.1f},{:.1f},{:.1f}) IN {}"
					" | Zrange={}",
					zln.id,
					dest_zone_short.c_str(),
					zln.target_zone_id,
					zln.target_x, zln.target_y, zln.target_z,
					dest_zone_short.c_str(),
					zln.Zrange
				).c_str()
			);

			if (!reverse_rows.empty()) {
				const auto& rev = reverse_rows[0];
				// Trilogy client can't render saylinks; print the #goto
				// command as plain text the GM types manually. Also print
				// the fix command explicitly for the same reason.
				c->Message(
					Chat::White,
					fmt::format(
						"    door in [{}] is approx ({:.1f},{:.1f},{:.1f})"
						" - type:  #goto {:.0f} {:.0f} {:.0f}",
						zone->GetShortName(),
						rev.target_x, rev.target_y, rev.target_z,
						rev.target_x, rev.target_y, rev.target_z
					).c_str()
				);
				c->Message(
					Chat::White,
					fmt::format(
						"    then walk to the door face and type:  #fixzoneline {}",
						zln.id
					).c_str()
				);
			}
			else {
				c->Message(
					Chat::White,
					fmt::format(
						"    no reverse-pair hint available; find the door on foot,"
						" then type:  #fixzoneline {}",
						zln.id
					).c_str()
				);
			}
		}
		else {
			c->Message(
				Chat::White,
				fmt::format(
					"[  OK   id={}] -> {} ({}) : src ({:.1f}, {:.1f}, {:.1f})"
					" -> dest ({:.1f}, {:.1f}, {:.1f}) | Zrange={} mode={}",
					zln.id,
					dest_zone_short.c_str(),
					zln.target_zone_id,
					zln.x, zln.y, zln.z,
					zln.target_x, zln.target_y, zln.target_z,
					zln.Zrange,
					(int) zln.UseNewZoning
				).c_str()
			);
		}
	}

	c->SendChatLineBreak();
	c->Message(
		Chat::White,
		fmt::format(
			"[#zonelines] Summary: {} usable, {} broken. Your position: ({:.2f}, {:.2f}, {:.2f}).",
			usable_count, broken_count,
			c->GetX(), c->GetY(), c->GetZ()
		).c_str()
	);
}
