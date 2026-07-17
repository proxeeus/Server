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
#include <cmath>

// #fixzoneplane <id> <width>
//
// Converts a trilogy_zone_points row into plane-crossing mode (UseNewZoning
// 1 or 2) — the right shape for mid-sized outdoor boundaries where a single
// door-sized box (mode 0) is too narrow but the full seamless plane needs
// perpendicular-window clamps (MinVert/MaxVert).
//
// UX intent: capture the boundary geometry from where the GM is standing +
// facing, so there's no manual "which axis, what sign, what CenterPoint"
// arithmetic. Face through the opening toward the destination zone, run the
// command with the row id and how wide the opening is.
//
// Mode is auto-picked from the GM's heading:
//   - facing N/S (heading near 0/512 or 256) -> mode 2 (Y-plane crossing)
//   - facing E/W (heading near 128 or 384)   -> mode 1 (X-plane crossing)
//
// The boundary coord = GM's current position on the crossing axis. The
// perpendicular window (MinVert/MaxVert) is split around the GM's current
// position on the along-boundary axis, so the opening is CENTERED on where
// the GM stands. keepX / keepY is set for the along-boundary axis so the
// crossing carries seamlessly into the destination.
//
// SIGN CAVEAT: the plane-crossing branch fires when
//   row.y >= 0 -> GetY() >= row.y (crossing to higher Y)
//   row.y <= 0 -> GetY() <= row.y (crossing to lower Y)
// (mode 1 same with X). The command captures the boundary coord with its
// natural sign, which is correct when the crossing direction matches the
// coord's sign — the common EQClassic layout. If the coord sign doesn't
// match the crossing direction, the fire branch won't trigger; the user
// has to manually flip the sign via SQL. The command flags this at the end.
//
// The paired reverse-side row (destination zone -> this zone) needs the same
// treatment on the other side so seamless carry (dest_CenterPoint / MinVert /
// MaxVert resolution) works. The command reminds the user of that.

void command_fixzoneplane(Client *c, const Seperator *sep)
{
	if (!c || !zone) {
		return;
	}

	if (sep->argnum < 2 || !sep->arg[1] || !*sep->arg[1] || !sep->arg[2] || !*sep->arg[2]) {
		c->Message(Chat::White, "Usage: #fixzoneplane <id> <width>");
		c->Message(Chat::White, "  Stand at the CENTER of the opening, FACE through the boundary");
		c->Message(Chat::White, "  toward the destination zone, then run the command with the row");
		c->Message(Chat::White, "  id (from #zonelines) and the visual width of the opening in units.");
		c->Message(Chat::White, "  Facing picks mode: N/S = mode 2 (Y-plane), E/W = mode 1 (X-plane).");
		return;
	}

	if (!Strings::IsNumber(sep->arg[1]) || !Strings::IsNumber(sep->arg[2])) {
		c->Message(Chat::White, "Both arguments must be numeric. Usage: #fixzoneplane <id> <width>");
		return;
	}

	const int32 target_id = Strings::ToInt(sep->arg[1]);
	const int32 width     = Strings::ToInt(sep->arg[2]);

	if (width <= 0) {
		c->Message(Chat::White, "[#fixzoneplane] Width must be a positive integer.");
		return;
	}

	// Find row in current zone.
	TrilogyZoneLineNode* target_row = nullptr;
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
				"[#fixzoneplane] No row with id={} in zone [{}]'s trilogy_zone_points."
				" Run #zonelines to see valid ids for this zone.",
				target_id, zone->GetShortName()
			).c_str()
		);
		return;
	}

	// Determine plane mode from facing direction.
	// EQEmu heading 0-512:
	//   0/512  = north (+Y)      256 = south (-Y)
	//   128    = west  (+X)      384 = east  (-X)
	// Nearest cardinal (N/S vs E/W) picks crossing axis.
	float h = c->GetHeading();
	while (h < 0.0f)      h += 512.0f;
	while (h >= 512.0f)   h -= 512.0f;

	const float dist_north = std::min(h, 512.0f - h);
	const float dist_south = std::fabs(h - 256.0f);
	const float dist_ns    = std::min(dist_north, dist_south);

	const float dist_west  = std::fabs(h - 128.0f);
	const float dist_east  = std::fabs(h - 384.0f);
	const float dist_ew    = std::min(dist_west, dist_east);

	const bool facing_ns   = (dist_ns <= dist_ew);
	const int8 new_mode    = facing_ns ? 2 : 1;

	// Diagonal warning: heading closer than 16u to a 45-degree cardinal split.
	if (std::fabs(dist_ns - dist_ew) < 16.0f) {
		c->Message(
			Chat::Yellow,
			fmt::format(
				"[#fixzoneplane] WARNING: your heading ({:.0f}) is roughly diagonal."
				" Plane mode assumes an axis-aligned boundary — face more directly"
				" N/S or E/W and rerun for a cleaner encoding.",
				h
			).c_str()
		);
	}

	// Capture geometry.
	const float half_width  = static_cast<float>(width) / 2.0f;

	float new_x       = target_row->x;
	float new_y       = target_row->y;
	float new_center  = 0.0f;
	float new_minvert = 0.0f;
	float new_maxvert = 0.0f;
	int32 new_keepX   = 0;
	int32 new_keepY   = 0;
	int32 new_keepZ   = 0;

	// keepX/keepY are deliberately set to 0 so the fire path uses the
	// CenterPoint remap (CROSS-COORD-FRAME position-preserving):
	//   sendX = dest_CenterPoint + (GetX() - CenterPoint), clamped to
	//   dest_MinVert/MaxVert. Player at this-side opening center -> lands at
	//   destination opening center; off-center by dx -> off-center by dx.
	// keepX=1 (raw carry with clamp) would ONLY work when the two zones
	// literally share the same coord frame value-for-value, which is not
	// the common case for imported EQClassic zone pairs. Setting keepX=0
	// works correctly whether the two zones share coords or not (offset
	// cancels out when CenterPoint == dest_CenterPoint).
	if (new_mode == 2) {
		// Y-plane: boundary at row.y, window on X, X remaps via CenterPoint.
		new_y       = c->GetY();
		new_center  = c->GetX();
		new_minvert = c->GetX() - half_width;
		new_maxvert = c->GetX() + half_width;
		new_keepX   = 0;
	}
	else {
		// X-plane: boundary at row.x, window on Y, Y remaps via CenterPoint.
		new_x       = c->GetX();
		new_center  = c->GetY();
		new_minvert = c->GetY() - half_width;
		new_maxvert = c->GetY() + half_width;
		new_keepY   = 0;
	}
	constexpr int32 new_zrange = 0; // unused in plane modes

	// Persist to DB first.
	const std::string update_q = fmt::format(
		"UPDATE trilogy_zone_points SET"
		" UseNewZoning = {}, x = {:.2f}, y = {:.2f},"
		" CenterPoint = {:.2f}, MinVert = {:.2f}, MaxVert = {:.2f},"
		" keepX = {}, keepY = {}, keepZ = {},"
		" Zrange = {}"
		" WHERE id = {}",
		new_mode, new_x, new_y,
		new_center, new_minvert, new_maxvert,
		new_keepX, new_keepY, new_keepZ,
		new_zrange,
		target_id
	);
	auto results = content_db.QueryDatabase(update_q);
	if (!results.Success()) {
		c->Message(
			Chat::Red,
			fmt::format(
				"[#fixzoneplane] DB UPDATE failed for id={}: {}",
				target_id, results.ErrorMessage()
			).c_str()
		);
		return;
	}

	// Live in-memory update — effective on next tick.
	target_row->UseNewZoning = new_mode;
	target_row->x            = new_x;
	target_row->y            = new_y;
	target_row->CenterPoint  = new_center;
	target_row->MinVert      = new_minvert;
	target_row->MaxVert      = new_maxvert;
	target_row->keepX        = new_keepX;
	target_row->keepY        = new_keepY;
	target_row->keepZ        = new_keepZ;
	target_row->Zrange       = new_zrange;

	const std::string dest_zone_short = ZoneName(target_row->target_zone_id, true);

	c->Message(
		Chat::Green,
		fmt::format(
			"[#fixzoneplane] Row id={} -> mode {} ({}-plane): boundary at {}={:.1f},"
			" window {}=[{:.1f}, {:.1f}] (width {}, center {:.1f}).",
			target_id, new_mode,
			(new_mode == 2) ? "Y" : "X",
			(new_mode == 2) ? "Y" : "X",
			(new_mode == 2) ? c->GetY() : c->GetX(),
			(new_mode == 2) ? "X" : "Y",
			new_minvert, new_maxvert,
			width, new_center
		).c_str()
	);
	c->Message(
		Chat::White,
		fmt::format(
			"[#fixzoneplane] Now run the same command on the paired row in [{}]"
			" (stand at that side's opening, face back through toward here). If the"
			" boundary doesn't fire when you walk across, the coord sign may not"
			" match the crossing direction — flip the boundary coord sign via SQL"
			" (UPDATE trilogy_zone_points SET {}=-{} WHERE id={}).",
			dest_zone_short.c_str(),
			(new_mode == 2) ? "y" : "x",
			(new_mode == 2) ? "y" : "x",
			target_id
		).c_str()
	);

	LogInfo(
		"[TrilogyZP] #fixzoneplane applied by {} (charid={}): row id={}"
		" zone=[{}] -> [{}] mode={} boundary=({:.2f},{:.2f})"
		" center={:.2f} minvert={:.2f} maxvert={:.2f}"
		" keepX={} keepY={} keepZ={} heading={:.1f}",
		c->GetCleanName(), c->CharacterID(),
		target_id,
		zone->GetShortName(), dest_zone_short.c_str(),
		new_mode, new_x, new_y,
		new_center, new_minvert, new_maxvert,
		new_keepX, new_keepY, new_keepZ,
		h
	);
}
