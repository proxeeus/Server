/**
 * DO NOT MODIFY THIS FILE
 *
 * This repository was automatically generated and is NOT to be modified directly.
 * Any repository modifications are meant to be made to the repository extending the base.
 * Any modifications to base repositories are to be made by the generator only
 *
 * @generator ./utils/scripts/generators/repository-generator.pl
 * @docs https://eqemu.gitbook.io/server/in-development/developer-area/repositories
 */

#ifndef EQEMU_BASE_BUG_REPORTS_REPOSITORY_H
#define EQEMU_BASE_BUG_REPORTS_REPOSITORY_H

#include "../../database.h"
#include "../../strings.h"
#include <ctime>

class BaseBugReportsRepository {
public:
	struct BugReports {
		int         id;
		std::string zone;
		int         client_version_id;
		std::string client_version_name;
		int         account_id;
		int         character_id;
		std::string character_name;
		int         reporter_spoof;
		int         category_id;
		std::string category_name;
		std::string reporter_name;
		std::string ui_path;
		float       pos_x;
		float       pos_y;
		float       pos_z;
		int         heading;
		int         time_played;
		int         target_id;
		std::string target_name;
		int         optional_info_mask;
		int         _can_duplicate;
		int         _crash_bug;
		int         _target_info;
		int         _character_flags;
		int         _unknown_value;
		std::string bug_report;
		std::string system_info;
		time_t      report_datetime;
		int         bug_status;
		time_t      last_review;
		std::string last_reviewer;
		std::string reviewer_notes;
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
			"client_version_id",
			"client_version_name",
			"account_id",
			"character_id",
			"character_name",
			"reporter_spoof",
			"category_id",
			"category_name",
			"reporter_name",
			"ui_path",
			"pos_x",
			"pos_y",
			"pos_z",
			"heading",
			"time_played",
			"target_id",
			"target_name",
			"optional_info_mask",
			"_can_duplicate",
			"_crash_bug",
			"_target_info",
			"_character_flags",
			"_unknown_value",
			"bug_report",
			"system_info",
			"report_datetime",
			"bug_status",
			"last_review",
			"last_reviewer",
			"reviewer_notes",
		};
	}

	static std::vector<std::string> SelectColumns()
	{
		return {
			"id",
			"zone",
			"client_version_id",
			"client_version_name",
			"account_id",
			"character_id",
			"character_name",
			"reporter_spoof",
			"category_id",
			"category_name",
			"reporter_name",
			"ui_path",
			"pos_x",
			"pos_y",
			"pos_z",
			"heading",
			"time_played",
			"target_id",
			"target_name",
			"optional_info_mask",
			"_can_duplicate",
			"_crash_bug",
			"_target_info",
			"_character_flags",
			"_unknown_value",
			"bug_report",
			"system_info",
			"UNIX_TIMESTAMP(report_datetime)",
			"bug_status",
			"UNIX_TIMESTAMP(last_review)",
			"last_reviewer",
			"reviewer_notes",
		};
	}

	static std::string ColumnsRaw()
	{
		return std::string(Strings::Implode(", ", Columns()));
	}

	static std::string SelectColumnsRaw()
	{
		return std::string(Strings::Implode(", ", SelectColumns()));
	}

	static std::string TableName()
	{
		return std::string("bug_reports");
	}

	static std::string BaseSelect()
	{
		return fmt::format(
			"SELECT {} FROM {}",
			SelectColumnsRaw(),
			TableName()
		);
	}

	static std::string BaseInsert()
	{
		return fmt::format(
			"INSERT INTO {} ({}) ",
			TableName(),
			ColumnsRaw()
		);
	}

	static BugReports NewEntity()
	{
		BugReports e{};

		e.id                  = 0;
		e.zone                = "Unknown";
		e.client_version_id   = 0;
		e.client_version_name = "Unknown";
		e.account_id          = 0;
		e.character_id        = 0;
		e.character_name      = "Unknown";
		e.reporter_spoof      = 1;
		e.category_id         = 0;
		e.category_name       = "Other";
		e.reporter_name       = "Unknown";
		e.ui_path             = "Unknown";
		e.pos_x               = 0;
		e.pos_y               = 0;
		e.pos_z               = 0;
		e.heading             = 0;
		e.time_played         = 0;
		e.target_id           = 0;
		e.target_name         = "Unknown";
		e.optional_info_mask  = 0;
		e._can_duplicate      = 0;
		e._crash_bug          = 0;
		e._target_info        = 0;
		e._character_flags    = 0;
		e._unknown_value      = 0;
		e.bug_report          = "";
		e.system_info         = "";
		e.report_datetime     = std::time(nullptr);
		e.bug_status          = 0;
		e.last_review         = std::time(nullptr);
		e.last_reviewer       = "None";
		e.reviewer_notes      = "";

		return e;
	}

	static BugReports GetBugReports(
		const std::vector<BugReports> &bug_reportss,
		int bug_reports_id
	)
	{
		for (auto &bug_reports : bug_reportss) {
			if (bug_reports.id == bug_reports_id) {
				return bug_reports;
			}
		}

		return NewEntity();
	}

	static BugReports FindOne(
		Database& db,
		int bug_reports_id
	)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"{} WHERE id = {} LIMIT 1",
				BaseSelect(),
				bug_reports_id
			)
		);

		auto row = results.begin();
		if (results.RowCount() == 1) {
			BugReports e{};

			e.id                  = atoi(row[0]);
			e.zone                = row[1] ? row[1] : "";
			e.client_version_id   = atoi(row[2]);
			e.client_version_name = row[3] ? row[3] : "";
			e.account_id          = atoi(row[4]);
			e.character_id        = atoi(row[5]);
			e.character_name      = row[6] ? row[6] : "";
			e.reporter_spoof      = atoi(row[7]);
			e.category_id         = atoi(row[8]);
			e.category_name       = row[9] ? row[9] : "";
			e.reporter_name       = row[10] ? row[10] : "";
			e.ui_path             = row[11] ? row[11] : "";
			e.pos_x               = static_cast<float>(atof(row[12]));
			e.pos_y               = static_cast<float>(atof(row[13]));
			e.pos_z               = static_cast<float>(atof(row[14]));
			e.heading             = atoi(row[15]);
			e.time_played         = atoi(row[16]);
			e.target_id           = atoi(row[17]);
			e.target_name         = row[18] ? row[18] : "";
			e.optional_info_mask  = atoi(row[19]);
			e._can_duplicate      = atoi(row[20]);
			e._crash_bug          = atoi(row[21]);
			e._target_info        = atoi(row[22]);
			e._character_flags    = atoi(row[23]);
			e._unknown_value      = atoi(row[24]);
			e.bug_report          = row[25] ? row[25] : "";
			e.system_info         = row[26] ? row[26] : "";
			e.report_datetime     = strtoll(row[27] ? row[27] : "-1", nullptr, 10);
			e.bug_status          = atoi(row[28]);
			e.last_review         = strtoll(row[29] ? row[29] : "-1", nullptr, 10);
			e.last_reviewer       = row[30] ? row[30] : "";
			e.reviewer_notes      = row[31] ? row[31] : "";

			return e;
		}

		return NewEntity();
	}

	static int DeleteOne(
		Database& db,
		int bug_reports_id
	)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"DELETE FROM {} WHERE {} = {}",
				TableName(),
				PrimaryKey(),
				bug_reports_id
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int UpdateOne(
		Database& db,
		const BugReports &e
	)
	{
		std::vector<std::string> v;

		auto columns = Columns();

		v.push_back(columns[1] + " = '" + Strings::Escape(e.zone) + "'");
		v.push_back(columns[2] + " = " + std::to_string(e.client_version_id));
		v.push_back(columns[3] + " = '" + Strings::Escape(e.client_version_name) + "'");
		v.push_back(columns[4] + " = " + std::to_string(e.account_id));
		v.push_back(columns[5] + " = " + std::to_string(e.character_id));
		v.push_back(columns[6] + " = '" + Strings::Escape(e.character_name) + "'");
		v.push_back(columns[7] + " = " + std::to_string(e.reporter_spoof));
		v.push_back(columns[8] + " = " + std::to_string(e.category_id));
		v.push_back(columns[9] + " = '" + Strings::Escape(e.category_name) + "'");
		v.push_back(columns[10] + " = '" + Strings::Escape(e.reporter_name) + "'");
		v.push_back(columns[11] + " = '" + Strings::Escape(e.ui_path) + "'");
		v.push_back(columns[12] + " = " + std::to_string(e.pos_x));
		v.push_back(columns[13] + " = " + std::to_string(e.pos_y));
		v.push_back(columns[14] + " = " + std::to_string(e.pos_z));
		v.push_back(columns[15] + " = " + std::to_string(e.heading));
		v.push_back(columns[16] + " = " + std::to_string(e.time_played));
		v.push_back(columns[17] + " = " + std::to_string(e.target_id));
		v.push_back(columns[18] + " = '" + Strings::Escape(e.target_name) + "'");
		v.push_back(columns[19] + " = " + std::to_string(e.optional_info_mask));
		v.push_back(columns[20] + " = " + std::to_string(e._can_duplicate));
		v.push_back(columns[21] + " = " + std::to_string(e._crash_bug));
		v.push_back(columns[22] + " = " + std::to_string(e._target_info));
		v.push_back(columns[23] + " = " + std::to_string(e._character_flags));
		v.push_back(columns[24] + " = " + std::to_string(e._unknown_value));
		v.push_back(columns[25] + " = '" + Strings::Escape(e.bug_report) + "'");
		v.push_back(columns[26] + " = '" + Strings::Escape(e.system_info) + "'");
		v.push_back(columns[27] + " = FROM_UNIXTIME(" + (e.report_datetime > 0 ? std::to_string(e.report_datetime) : "null") + ")");
		v.push_back(columns[28] + " = " + std::to_string(e.bug_status));
		v.push_back(columns[29] + " = FROM_UNIXTIME(" + (e.last_review > 0 ? std::to_string(e.last_review) : "null") + ")");
		v.push_back(columns[30] + " = '" + Strings::Escape(e.last_reviewer) + "'");
		v.push_back(columns[31] + " = '" + Strings::Escape(e.reviewer_notes) + "'");

		auto results = db.QueryDatabase(
			fmt::format(
				"UPDATE {} SET {} WHERE {} = {}",
				TableName(),
				Strings::Implode(", ", v),
				PrimaryKey(),
				e.id
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static BugReports InsertOne(
		Database& db,
		BugReports e
	)
	{
		std::vector<std::string> v;

		v.push_back(std::to_string(e.id));
		v.push_back("'" + Strings::Escape(e.zone) + "'");
		v.push_back(std::to_string(e.client_version_id));
		v.push_back("'" + Strings::Escape(e.client_version_name) + "'");
		v.push_back(std::to_string(e.account_id));
		v.push_back(std::to_string(e.character_id));
		v.push_back("'" + Strings::Escape(e.character_name) + "'");
		v.push_back(std::to_string(e.reporter_spoof));
		v.push_back(std::to_string(e.category_id));
		v.push_back("'" + Strings::Escape(e.category_name) + "'");
		v.push_back("'" + Strings::Escape(e.reporter_name) + "'");
		v.push_back("'" + Strings::Escape(e.ui_path) + "'");
		v.push_back(std::to_string(e.pos_x));
		v.push_back(std::to_string(e.pos_y));
		v.push_back(std::to_string(e.pos_z));
		v.push_back(std::to_string(e.heading));
		v.push_back(std::to_string(e.time_played));
		v.push_back(std::to_string(e.target_id));
		v.push_back("'" + Strings::Escape(e.target_name) + "'");
		v.push_back(std::to_string(e.optional_info_mask));
		v.push_back(std::to_string(e._can_duplicate));
		v.push_back(std::to_string(e._crash_bug));
		v.push_back(std::to_string(e._target_info));
		v.push_back(std::to_string(e._character_flags));
		v.push_back(std::to_string(e._unknown_value));
		v.push_back("'" + Strings::Escape(e.bug_report) + "'");
		v.push_back("'" + Strings::Escape(e.system_info) + "'");
		v.push_back("FROM_UNIXTIME(" + (e.report_datetime > 0 ? std::to_string(e.report_datetime) : "null") + ")");
		v.push_back(std::to_string(e.bug_status));
		v.push_back("FROM_UNIXTIME(" + (e.last_review > 0 ? std::to_string(e.last_review) : "null") + ")");
		v.push_back("'" + Strings::Escape(e.last_reviewer) + "'");
		v.push_back("'" + Strings::Escape(e.reviewer_notes) + "'");

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES ({})",
				BaseInsert(),
				Strings::Implode(",", v)
			)
		);

		if (results.Success()) {
			e.id = results.LastInsertedID();
			return e;
		}

		e = NewEntity();

		return e;
	}

	static int InsertMany(
		Database& db,
		const std::vector<BugReports> &entries
	)
	{
		std::vector<std::string> insert_chunks;

		for (auto &e: entries) {
			std::vector<std::string> v;

			v.push_back(std::to_string(e.id));
			v.push_back("'" + Strings::Escape(e.zone) + "'");
			v.push_back(std::to_string(e.client_version_id));
			v.push_back("'" + Strings::Escape(e.client_version_name) + "'");
			v.push_back(std::to_string(e.account_id));
			v.push_back(std::to_string(e.character_id));
			v.push_back("'" + Strings::Escape(e.character_name) + "'");
			v.push_back(std::to_string(e.reporter_spoof));
			v.push_back(std::to_string(e.category_id));
			v.push_back("'" + Strings::Escape(e.category_name) + "'");
			v.push_back("'" + Strings::Escape(e.reporter_name) + "'");
			v.push_back("'" + Strings::Escape(e.ui_path) + "'");
			v.push_back(std::to_string(e.pos_x));
			v.push_back(std::to_string(e.pos_y));
			v.push_back(std::to_string(e.pos_z));
			v.push_back(std::to_string(e.heading));
			v.push_back(std::to_string(e.time_played));
			v.push_back(std::to_string(e.target_id));
			v.push_back("'" + Strings::Escape(e.target_name) + "'");
			v.push_back(std::to_string(e.optional_info_mask));
			v.push_back(std::to_string(e._can_duplicate));
			v.push_back(std::to_string(e._crash_bug));
			v.push_back(std::to_string(e._target_info));
			v.push_back(std::to_string(e._character_flags));
			v.push_back(std::to_string(e._unknown_value));
			v.push_back("'" + Strings::Escape(e.bug_report) + "'");
			v.push_back("'" + Strings::Escape(e.system_info) + "'");
			v.push_back("FROM_UNIXTIME(" + (e.report_datetime > 0 ? std::to_string(e.report_datetime) : "null") + ")");
			v.push_back(std::to_string(e.bug_status));
			v.push_back("FROM_UNIXTIME(" + (e.last_review > 0 ? std::to_string(e.last_review) : "null") + ")");
			v.push_back("'" + Strings::Escape(e.last_reviewer) + "'");
			v.push_back("'" + Strings::Escape(e.reviewer_notes) + "'");

			insert_chunks.push_back("(" + Strings::Implode(",", v) + ")");
		}

		std::vector<std::string> v;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES {}",
				BaseInsert(),
				Strings::Implode(",", insert_chunks)
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static std::vector<BugReports> All(Database& db)
	{
		std::vector<BugReports> all_entries;

		auto results = db.QueryDatabase(
			fmt::format(
				"{}",
				BaseSelect()
			)
		);

		all_entries.reserve(results.RowCount());

		for (auto row = results.begin(); row != results.end(); ++row) {
			BugReports e{};

			e.id                  = atoi(row[0]);
			e.zone                = row[1] ? row[1] : "";
			e.client_version_id   = atoi(row[2]);
			e.client_version_name = row[3] ? row[3] : "";
			e.account_id          = atoi(row[4]);
			e.character_id        = atoi(row[5]);
			e.character_name      = row[6] ? row[6] : "";
			e.reporter_spoof      = atoi(row[7]);
			e.category_id         = atoi(row[8]);
			e.category_name       = row[9] ? row[9] : "";
			e.reporter_name       = row[10] ? row[10] : "";
			e.ui_path             = row[11] ? row[11] : "";
			e.pos_x               = static_cast<float>(atof(row[12]));
			e.pos_y               = static_cast<float>(atof(row[13]));
			e.pos_z               = static_cast<float>(atof(row[14]));
			e.heading             = atoi(row[15]);
			e.time_played         = atoi(row[16]);
			e.target_id           = atoi(row[17]);
			e.target_name         = row[18] ? row[18] : "";
			e.optional_info_mask  = atoi(row[19]);
			e._can_duplicate      = atoi(row[20]);
			e._crash_bug          = atoi(row[21]);
			e._target_info        = atoi(row[22]);
			e._character_flags    = atoi(row[23]);
			e._unknown_value      = atoi(row[24]);
			e.bug_report          = row[25] ? row[25] : "";
			e.system_info         = row[26] ? row[26] : "";
			e.report_datetime     = strtoll(row[27] ? row[27] : "-1", nullptr, 10);
			e.bug_status          = atoi(row[28]);
			e.last_review         = strtoll(row[29] ? row[29] : "-1", nullptr, 10);
			e.last_reviewer       = row[30] ? row[30] : "";
			e.reviewer_notes      = row[31] ? row[31] : "";

			all_entries.push_back(e);
		}

		return all_entries;
	}

	static std::vector<BugReports> GetWhere(Database& db, const std::string &where_filter)
	{
		std::vector<BugReports> all_entries;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} WHERE {}",
				BaseSelect(),
				where_filter
			)
		);

		all_entries.reserve(results.RowCount());

		for (auto row = results.begin(); row != results.end(); ++row) {
			BugReports e{};

			e.id                  = atoi(row[0]);
			e.zone                = row[1] ? row[1] : "";
			e.client_version_id   = atoi(row[2]);
			e.client_version_name = row[3] ? row[3] : "";
			e.account_id          = atoi(row[4]);
			e.character_id        = atoi(row[5]);
			e.character_name      = row[6] ? row[6] : "";
			e.reporter_spoof      = atoi(row[7]);
			e.category_id         = atoi(row[8]);
			e.category_name       = row[9] ? row[9] : "";
			e.reporter_name       = row[10] ? row[10] : "";
			e.ui_path             = row[11] ? row[11] : "";
			e.pos_x               = static_cast<float>(atof(row[12]));
			e.pos_y               = static_cast<float>(atof(row[13]));
			e.pos_z               = static_cast<float>(atof(row[14]));
			e.heading             = atoi(row[15]);
			e.time_played         = atoi(row[16]);
			e.target_id           = atoi(row[17]);
			e.target_name         = row[18] ? row[18] : "";
			e.optional_info_mask  = atoi(row[19]);
			e._can_duplicate      = atoi(row[20]);
			e._crash_bug          = atoi(row[21]);
			e._target_info        = atoi(row[22]);
			e._character_flags    = atoi(row[23]);
			e._unknown_value      = atoi(row[24]);
			e.bug_report          = row[25] ? row[25] : "";
			e.system_info         = row[26] ? row[26] : "";
			e.report_datetime     = strtoll(row[27] ? row[27] : "-1", nullptr, 10);
			e.bug_status          = atoi(row[28]);
			e.last_review         = strtoll(row[29] ? row[29] : "-1", nullptr, 10);
			e.last_reviewer       = row[30] ? row[30] : "";
			e.reviewer_notes      = row[31] ? row[31] : "";

			all_entries.push_back(e);
		}

		return all_entries;
	}

	static int DeleteWhere(Database& db, const std::string &where_filter)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"DELETE FROM {} WHERE {}",
				TableName(),
				where_filter
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int Truncate(Database& db)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"TRUNCATE TABLE {}",
				TableName()
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int64 GetMaxId(Database& db)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"SELECT COALESCE(MAX({}), 0) FROM {}",
				PrimaryKey(),
				TableName()
			)
		);

		return (results.Success() && results.begin()[0] ? strtoll(results.begin()[0], nullptr, 10) : 0);
	}

	static int64 Count(Database& db, const std::string &where_filter = "")
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"SELECT COUNT(*) FROM {} {}",
				TableName(),
				(where_filter.empty() ? "" : "WHERE " + where_filter)
			)
		);

		return (results.Success() && results.begin()[0] ? strtoll(results.begin()[0], nullptr, 10) : 0);
	}

};

#endif //EQEMU_BASE_BUG_REPORTS_REPOSITORY_H
