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

	// Argument parsing has TWO forms:
	//
	//   #fixzoneheading             (no args)
	//       Auto-pick mode. Finds ANY row whose target_zone = current zone
	//       and whose target coord is closest to the GM's current position.
	//       That's the row that FIRED to bring the GM here. Works out of the
	//       box — no need to think about direction or source zone. Just walk
	//       in, turn, type the command.
	//
	//   #fixzoneheading <id>
	//       Direct row-id form. Override when auto-pick guesses wrong (rare,
	//       usually only when the GM has moved far from their arrival spot).
	//
	// Auto-pick is the common case (99%) — arrive, turn, run command. The id
	// form is the safety net.
	std::vector<TrilogyZonePointsRepository::TrilogyZonePoints> rows;
	const bool has_arg = (sep->argnum >= 1 && sep->arg[1] && *sep->arg[1]);

	if (has_arg) {
		// Any non-empty arg is treated as a row id. Rejecting non-numeric
		// input keeps the UX simple (single arg meaning) — no more confusion
		// about "source zone vs destination zone vs current zone".
		if (!Strings::IsNumber(sep->arg[1])) {
			c->Message(Chat::White, "Usage: #fixzoneheading            (auto-pick the row that brought you here)");
			c->Message(Chat::White, "       #fixzoneheading <id>       (override: specific trilogy_zone_points row id)");
			c->Message(Chat::White, "  Stand in the arrival zone facing the direction players should face.");
			return;
		}
		const int32 row_id = Strings::ToInt(sep->arg[1]);
		rows = TrilogyZonePointsRepository::GetWhere(
			content_db,
			fmt::format("id = {}", row_id)
		);
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
	}
	else {
		// Auto-pick: find the row that brought us here.
		//
		// Signal: the row that fired has target_zone = current zone AND its
		// target coord ≈ our current position (that's where we landed). We
		// pick the row targeting our current zone whose target is closest to
		// our current position, regardless of source zone. Works for droga
		// arriving from nurga (multiple nurga rows), from frontiermtns, from
		// anywhere — the proximity match ignores source.
		//
		// Handy for multi-source destinations: velketor might have rows from
		// greatdivide, thurgadin, etc. all landing at different spots — the
		// row that fired is the one whose target matches where we stand.
		if (!zone) {
			c->Message(Chat::White, "[#fixzoneheading] No current zone context - cannot auto-pick.");
			return;
		}
		rows = TrilogyZonePointsRepository::GetWhere(
			content_db,
			fmt::format(
				"target_zone = '{}'"
				" ORDER BY (POW(target_x - {}, 2) + POW(target_y - {}, 2) + POW(target_z - {}, 2)) ASC"
				" LIMIT 1",
				Strings::Escape(zone->GetShortName()),
				c->GetX(), c->GetY(), c->GetZ()
			)
		);
		if (rows.empty()) {
			c->Message(
				Chat::White,
				fmt::format(
					"[#fixzoneheading] No trilogy_zone_points row targets [{}]."
					" Nothing to update.",
					zone->GetShortName()
				).c_str()
			);
			return;
		}
		// Report the pick + confidence so the GM can sanity-check.
		const auto& picked = rows[0];
		const float dist_2d = std::sqrt(
			(picked.target_x - c->GetX()) * (picked.target_x - c->GetX())
			+ (picked.target_y - c->GetY()) * (picked.target_y - c->GetY())
		);
		c->Message(
			Chat::White,
			fmt::format(
				"[#fixzoneheading] Auto-picked row id={} : from [{}] to [{}], target"
				" ({:.1f},{:.1f},{:.1f}) is {:.0f}u from your position.",
				picked.id, picked.zone, picked.target_zone,
				picked.target_x, picked.target_y, picked.target_z, dist_2d
			).c_str()
		);
		if (dist_2d > 100.0f) {
			c->Message(
				Chat::Yellow,
				fmt::format(
					"[#fixzoneheading] WARNING: pick is {:.0f}u from your position - probably not the row you meant."
					" Walk closer to the arrival spot and rerun, or use #fixzoneheading <id> with a specific id.",
					dist_2d
				).c_str()
			);
		}
	}

	const auto&  row    = rows[0];
	const int32  row_id = row.id;

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
