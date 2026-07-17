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

// Base repository for the `trilogy_zone_points` table.
//
// This table is a direct EQClassic import — schema matches EQClassic's
// `zone_points` table byte-for-byte, imported into a separate `trilogy_`
// prefixed table so the Trilogy client's zoning code path is completely
// isolated from EQEmu's modern-client zoning. See MEMORY.md
// (project_trilogy_zone_transition) for the architecture rationale.
//
// This file is intentionally hand-written rather than emitted by the EQEmu
// repository generator: the Trilogy table is read-only from the server's
// perspective (populated once by the operator via SQL import, never mutated
// at runtime), so we only need the read path — none of the Insert/Update/
// Delete/InsertMany scaffolding the generator produces. Column list, struct
// layout, and typed accessors follow the generator's conventions so it looks
// familiar to anyone who's edited a generated base repository.

#ifndef EQEMU_BASE_TRILOGY_ZONE_POINTS_REPOSITORY_H
#define EQEMU_BASE_TRILOGY_ZONE_POINTS_REPOSITORY_H

#include "../../database.h"
#include "../../strings.h"
#include <ctime>

class BaseTrilogyZonePointsRepository {
public:
	// Direct 1:1 mirror of the EQClassic `zone_points` table schema. Field
	// order matches the SQL column order in the imported dump so the SelectAll
	// path lines up cleanly with the row-index reads in GetWhere().
	struct TrilogyZonePoints {
		int32_t     id;
		std::string zone;
		float       y;
		float       x;
		float       z;
		float       heading;
		float       target_y;
		float       target_x;
		float       target_z;
		std::string target_zone;
		int32_t     keepX;
		int32_t     keepY;
		int32_t     keepZ;
		int32_t     maxZDiff;
		int32_t     Zrange;
		int8_t      UseNewZoning;   // 0 = old-mode box, 1 = X plane, 2 = Y plane
		float       CenterPoint;
		float       MaxVert;
		float       MinVert;
		int32_t     ToZoneID;
	};

	static std::string PrimaryKey()
	{
		return std::string("id");
	}

	static std::vector<std::string> Columns()
	{
		return {
			"id",
			"zone",
			"y",
			"x",
			"z",
			"heading",
			"target_y",
			"target_x",
			"target_z",
			"target_zone",
			"keepX",
			"keepY",
			"keepZ",
			"maxZDiff",
			"Zrange",
			"UseNewZoning",
			"CenterPoint",
			"MaxVert",
			"MinVert",
			"ToZoneID",
		};
	}

	static std::string ColumnsRaw()
	{
		return std::string(Strings::Implode(", ", Columns()));
	}

	static std::string TableName()
	{
		return std::string("trilogy_zone_points");
	}

	static std::string BaseSelect()
	{
		return fmt::format(
			"SELECT {} FROM {}",
			ColumnsRaw(),
			TableName()
		);
	}

	static TrilogyZonePoints NewEntity()
	{
		TrilogyZonePoints e{};

		e.id           = 0;
		e.zone         = "";
		e.y            = 0.0f;
		e.x            = 0.0f;
		e.z            = 0.0f;
		e.heading      = 0.0f;
		e.target_y     = 0.0f;
		e.target_x     = 0.0f;
		e.target_z     = 0.0f;
		e.target_zone  = "";
		e.keepX        = 0;
		e.keepY        = 0;
		e.keepZ        = 0;
		e.maxZDiff     = 0;
		e.Zrange       = 15; // matches the EQClassic DB DEFAULT
		e.UseNewZoning = 0;
		e.CenterPoint  = 0.0f;
		e.MaxVert      = 0.0f;
		e.MinVert      = 0.0f;
		e.ToZoneID     = 0;

		return e;
	}

	static std::vector<TrilogyZonePoints> GetWhere(Database& db, std::string where_filter)
	{
		std::vector<TrilogyZonePoints> all_entries;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} WHERE {}",
				BaseSelect(),
				where_filter
			)
		);

		all_entries.reserve(results.RowCount());

		for (auto row = results.begin(); row != results.end(); ++row) {
			TrilogyZonePoints e{};

			// Row indices match the Columns() vector order above.
			e.id           = row[0]  ? static_cast<int32_t>(atoi(row[0]))              : 0;
			e.zone         = row[1]  ? row[1]                                          : "";
			e.y            = row[2]  ? strtof(row[2],  nullptr)                        : 0.0f;
			e.x            = row[3]  ? strtof(row[3],  nullptr)                        : 0.0f;
			e.z            = row[4]  ? strtof(row[4],  nullptr)                        : 0.0f;
			e.heading      = row[5]  ? strtof(row[5],  nullptr)                        : 0.0f;
			e.target_y     = row[6]  ? strtof(row[6],  nullptr)                        : 0.0f;
			e.target_x     = row[7]  ? strtof(row[7],  nullptr)                        : 0.0f;
			e.target_z     = row[8]  ? strtof(row[8],  nullptr)                        : 0.0f;
			e.target_zone  = row[9]  ? row[9]                                          : "";
			e.keepX        = row[10] ? static_cast<int32_t>(atoi(row[10]))             : 0;
			e.keepY        = row[11] ? static_cast<int32_t>(atoi(row[11]))             : 0;
			e.keepZ        = row[12] ? static_cast<int32_t>(atoi(row[12]))             : 0;
			e.maxZDiff     = row[13] ? static_cast<int32_t>(atoi(row[13]))             : 0;
			e.Zrange       = row[14] ? static_cast<int32_t>(atoi(row[14]))             : 15;
			e.UseNewZoning = row[15] ? static_cast<int8_t>(atoi(row[15]))              : 0;
			e.CenterPoint  = row[16] ? strtof(row[16], nullptr)                        : 0.0f;
			e.MaxVert      = row[17] ? strtof(row[17], nullptr)                        : 0.0f;
			e.MinVert      = row[18] ? strtof(row[18], nullptr)                        : 0.0f;
			e.ToZoneID     = row[19] ? static_cast<int32_t>(atoi(row[19]))             : 0;

			all_entries.push_back(e);
		}

		return all_entries;
	}

	static std::vector<TrilogyZonePoints> All(Database& db)
	{
		return GetWhere(db, "1");
	}
};

#endif //EQEMU_BASE_TRILOGY_ZONE_POINTS_REPOSITORY_H
