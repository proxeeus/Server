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

// #fixzoneheading <id>
//
// Sets the ARRIVAL HEADING on a trilogy_zone_points row to the GM's current
// facing direction. The `heading` column represents which way the player
// faces when they LAND in the destination zone after zoning, so the intended
// workflow is:
//
//   1. Zone into the DESTINATION (the zone this line's target_zone points to)
//   2. Walk to where players will land (near the return door)
//   3. Face the direction players should be facing after arrival
//      (typically away from the door / into the zone)
//   4. #fixzoneheading <id>          <-- id is the SOURCE-zone row's id
//
// The row can belong to any zone — the id is a global trilogy_zone_points
// primary key. If the row is currently loaded in THIS zone's in-memory
// trilogy_zone_line_list (i.e. the GM is standing in the source zone), we
// also update the live copy so the fix is instantly effective for Trilogy
// players zoning right now. If the row belongs to another zone, only the DB
// is updated; that zone will pick up the new heading on its next
// #reload static or process restart.
//
// Heading conversion: EQClassic stores heading in the 0-255 wire scale
// (players face +Y=north at 0, +X=west at 64, south at 128, east at 192).
// Our runtime uses the EQEmu 0-512 internal scale (double resolution).
// GM's current heading (from GetHeading()) is in EQEmu scale; we divide by
// 2 to convert to the EQClassic wire scale for storage. CheckTrilogyZoneLines
// then multiplies by 2 at fire time to recover the EQEmu heading.

void command_fixzoneheading(Client *c, const Seperator *sep)
{
	if (!c) {
		return;
	}

	if (sep->argnum < 1 || !sep->arg[1] || !*sep->arg[1] || !Strings::IsNumber(sep->arg[1])) {
		c->Message(Chat::White, "Usage: #fixzoneheading <id>");
		c->Message(Chat::White, "  id is a trilogy_zone_points primary key (from #zonelines output).");
		c->Message(Chat::White, "  You should be standing IN THE DESTINATION zone, facing the direction");
		c->Message(Chat::White, "  players should face after arriving. The id refers to the SOURCE-zone row.");
		return;
	}

	const int32 row_id = Strings::ToInt(sep->arg[1]);

	// Look up the row directly from the DB — it may belong to any zone, not
	// just the one we're standing in. This is intentional because the natural
	// workflow puts the GM in the destination zone when capturing arrival
	// heading, but the row we're updating is in the source zone.
	const std::string query = fmt::format(
		"id = {}", row_id
	);
	auto rows = TrilogyZonePointsRepository::GetWhere(content_db, query);
	if (rows.empty()) {
		c->Message(
			Chat::White,
			fmt::format(
				"[#fixzoneheading] No trilogy_zone_points row with id={}.",
				row_id
			).c_str()
		);
		return;
	}

	const auto& row = rows[0];

	// Convert GM heading from EQEmu 0-512 to EQClassic 0-255 wire scale.
	// Divide by 2 and snap to [0, 255].
	const float eqemu_heading   = c->GetHeading();
	float       classic_heading = eqemu_heading * 0.5f;
	if (classic_heading < 0.0f)   classic_heading = 0.0f;
	if (classic_heading > 255.0f) classic_heading = 255.0f;

	// Persist to DB. Use float precision (2 decimals) — the column is float
	// so we don't need to round to integer.
	const std::string update_q = fmt::format(
		"UPDATE trilogy_zone_points SET heading = {:.2f} WHERE id = {}",
		classic_heading, row_id
	);
	auto results = content_db.QueryDatabase(update_q);
	if (!results.Success()) {
		c->Message(
			Chat::Red,
			fmt::format(
				"[#fixzoneheading] DB UPDATE failed for id={}: {}",
				row_id, results.ErrorMessage()
			).c_str()
		);
		return;
	}

	// If the row is loaded in the CURRENT zone's runtime list, update the
	// in-memory copy too so the fix is live immediately.
	bool live_update_applied = false;
	if (zone) {
		for (auto &zln : zone->trilogy_zone_line_list) {
			if (zln.id == row_id) {
				zln.heading         = classic_heading;
				live_update_applied = true;
				break;
			}
		}
	}

	c->Message(
		Chat::Green,
		fmt::format(
			"[#fixzoneheading] Row id={} (zone [{}] -> [{}]) heading updated to"
			" {:.2f} (EQClassic 0-255 scale, from your current heading {:.1f}"
			" in EQEmu 0-512 scale). {}",
			row_id,
			row.zone, row.target_zone,
			classic_heading, eqemu_heading,
			live_update_applied
				? "In-memory copy updated too - live now."
				: "Row belongs to another zone - takes effect on that zone's next #reload static or restart."
		).c_str()
	);

	LogInfo(
		"[TrilogyZP] #fixzoneheading applied by {} (charid={}): row id={}"
		" zone=[{}] -> [{}] heading={:.2f} (was {:.2f})"
		" GM current heading in EQEmu 0-512 scale={:.1f} live_update={}",
		c->GetCleanName(), c->CharacterID(),
		row_id, row.zone, row.target_zone,
		classic_heading, row.heading,
		eqemu_heading,
		live_update_applied ? "Y" : "N"
	);
}
