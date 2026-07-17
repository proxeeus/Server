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

// Extension repository for the trilogy_zone_points table.
// See the header of base_trilogy_zone_points_repository.h for the isolation
// rationale (Trilogy zoning is fully separated from EQEmu modern-client
// zoning). Custom queries live here; the base holds struct + generic CRUD.

#ifndef EQEMU_TRILOGY_ZONE_POINTS_REPOSITORY_H
#define EQEMU_TRILOGY_ZONE_POINTS_REPOSITORY_H

#include "../database.h"
#include "../strings.h"
#include "base/base_trilogy_zone_points_repository.h"

class TrilogyZonePointsRepository: public BaseTrilogyZonePointsRepository {
public:

	// Zoneline lookup by source zone shortname. Matches the EQClassic loader
	// query pattern (loadZoneLines in EQClassic/Common/Source/database.cpp).
	// Returns every row where zone = <shortname>. Called once at zone
	// initialization from ZoneDatabase::LoadTrilogyZonePoints.
	static std::vector<TrilogyZonePoints> GetByZone(Database& db, const std::string &zone_shortname)
	{
		return GetWhere(
			db,
			fmt::format("zone = '{}'", Strings::Escape(zone_shortname))
		);
	}

	// Paired-destination lookup for plane-crossing modes.
	//
	// EQClassic's runtime path issues per-fire DB queries against the paired
	// line (getTargetZoneCenter/Max/Min at
	// EQClassic/Common/Source/database.cpp:1380-1470). We do it once at
	// zone-load time and cache the three floats on the source
	// TrilogyZoneLineNode (see zone.h dest_CenterPoint/dest_MaxVert/dest_MinVert).
	//
	// Semantic mirror of EQClassic's query: the paired row is in the
	// destination zone (zone=<dest>), points BACK to us (target_zone=<source>),
	// carries our own line's id in its ToZoneID field, and is a plane-crossing
	// entry (UseNewZoning >= 1). Returns empty if no match, in which case the
	// loader leaves dest_resolved=false and the fire path falls back to raw
	// target coords.
	static std::vector<TrilogyZonePoints> GetPairedDestination(
		Database&          db,
		const std::string& source_zone_shortname,
		const std::string& destination_zone_shortname,
		int32_t            source_line_id
	)
	{
		return GetWhere(
			db,
			fmt::format(
				"zone = '{}' AND target_zone = '{}' AND ToZoneID = {} AND UseNewZoning >= 1",
				Strings::Escape(destination_zone_shortname),
				Strings::Escape(source_zone_shortname),
				source_line_id
			)
		);
	}
};

#endif //EQEMU_TRILOGY_ZONE_POINTS_REPOSITORY_H
