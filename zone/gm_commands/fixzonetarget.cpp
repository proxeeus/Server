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

// #fixzonetarget <id>
//
// Sets the ARRIVAL COORDINATES (target_x/y/z) of a trilogy_zone_points row to
// the GM's current position. Companion to #fixzoneline (which captures SOURCE
// coords) — this one captures WHERE PLAYERS LAND.
//
// Workflow:
//   1. Zone into the DESTINATION zone (the target of the row you want to fix)
//   2. Walk to where you want arriving players to land
//   3. #fixzonetarget <id>              <-- id is from the SOURCE zone's row,
//                                           found via #zonelines in the source
//
// Behaviour notes:
//   - The row can belong to ANY zone (this command updates a row by global id).
//     The DB write always happens. If the row is loaded in THIS zone's runtime
//     list (rare — the row's zone == current zone), we update the live copy so
//     the fix is instantly effective. Otherwise the source zone needs
//     `#reload static` or a restart to see the new coords.
//   - keepX and keepY are ALSO set to 0 by this command. Rationale: capturing
//     an explicit landing spot only makes sense if the row uses that spot, i.e.
//     no seamless coord carry from the source zone. If you want seamless carry
//     (ecommons↔nkarana style), use the plane-mode command instead; if you
//     want a fixed landing spot (dungeon door, non-seamless zone pair), that's
//     this command and keep=0 is what makes target coords authoritative.
//   - keepZ is left alone; Z carry is generally harmless.

void command_fixzonetarget(Client *c, const Seperator *sep)
{
	if (!c) {
		return;
	}

	if (sep->argnum < 1 || !sep->arg[1] || !*sep->arg[1]) {
		c->Message(Chat::White, "Usage: #fixzonetarget <id>");
		c->Message(Chat::White, "  Stand where you want arriving players to land, then run this");
		c->Message(Chat::White, "  with the SOURCE-zone row's id. Also clears keepX/keepY so the");
		c->Message(Chat::White, "  captured landing spot is authoritative (no seamless coord carry).");
		return;
	}

	if (!Strings::IsNumber(sep->arg[1])) {
		c->Message(Chat::White, "Argument must be numeric. Usage: #fixzonetarget <id>");
		return;
	}

	const int32 target_id = Strings::ToInt(sep->arg[1]);

	// Verify the row exists so we can print a useful confirmation message
	// including which source zone / target zone it describes.
	auto rows = TrilogyZonePointsRepository::GetWhere(
		content_db, fmt::format("id = {}", target_id)
	);
	if (rows.empty()) {
		c->Message(
			Chat::White,
			fmt::format(
				"[#fixzonetarget] No trilogy_zone_points row with id={}.",
				target_id
			).c_str()
		);
		return;
	}
	const auto& row = rows[0];

	const float new_target_x = c->GetX();
	const float new_target_y = c->GetY();
	const float new_target_z = c->GetZ();

	// Persist to DB first. Also clear keepX/keepY so the landing spot is used
	// verbatim rather than remapped via CenterPoint or clamped via MinVert/MaxVert.
	const std::string update_q = fmt::format(
		"UPDATE trilogy_zone_points SET"
		" target_x = {:.2f}, target_y = {:.2f}, target_z = {:.2f},"
		" keepX = 0, keepY = 0"
		" WHERE id = {}",
		new_target_x, new_target_y, new_target_z, target_id
	);
	auto results = content_db.QueryDatabase(update_q);
	if (!results.Success()) {
		c->Message(
			Chat::Red,
			fmt::format(
				"[#fixzonetarget] DB UPDATE failed for id={}: {}",
				target_id, results.ErrorMessage()
			).c_str()
		);
		return;
	}

	// If the row happens to also be loaded in THIS zone's runtime list
	// (source zone == current zone), update in-memory too so the fix is live
	// without any reload.
	bool live_update_applied = false;
	if (zone) {
		for (auto &zln : zone->trilogy_zone_line_list) {
			if (zln.id == target_id) {
				zln.target_x         = new_target_x;
				zln.target_y         = new_target_y;
				zln.target_z         = new_target_z;
				zln.keepX            = 0;
				zln.keepY            = 0;
				live_update_applied  = true;
				break;
			}
		}
	}

	c->Message(
		Chat::Green,
		fmt::format(
			"[#fixzonetarget] Row id={} ([{}] -> [{}]) landing coords set to"
			" ({:.2f}, {:.2f}, {:.2f}), keepX/keepY cleared. {}",
			target_id, row.zone, row.target_zone,
			new_target_x, new_target_y, new_target_z,
			live_update_applied
				? "In-memory copy updated - live now."
				: "Row belongs to another zone - takes effect on that zone's"
				  " next #reload static or restart."
		).c_str()
	);

	LogInfo(
		"[TrilogyZP] #fixzonetarget applied by {} (charid={}): row id={}"
		" zone=[{}] -> [{}] target=({:.2f},{:.2f},{:.2f}) keepX=0 keepY=0"
		" live_update={} (was target ({:.2f},{:.2f},{:.2f}) keepX={} keepY={})",
		c->GetCleanName(), c->CharacterID(),
		target_id, row.zone, row.target_zone,
		new_target_x, new_target_y, new_target_z,
		live_update_applied ? "Y" : "N",
		row.target_x, row.target_y, row.target_z,
		row.keepX, row.keepY
	);
}
