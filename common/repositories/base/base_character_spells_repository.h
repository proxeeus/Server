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

#ifndef EQEMU_BASE_CHARACTER_SPELLS_REPOSITORY_H
#define EQEMU_BASE_CHARACTER_SPELLS_REPOSITORY_H

#include "../../database.h"
#include "../../string_util.h"
#include <ctime>

class BaseCharacterSpellsRepository {
public:
	struct CharacterSpells {
		int id;
		int slot_id;
		int spell_id;
	};

	static std::string PrimaryKey()
	{
		return std::string("id");
	}

	static std::vector<std::string> Columns()
	{
		return {
			"id",
			"slot_id",
			"spell_id",
		};
	}

	static std::vector<std::string> SelectColumns()
	{
		return {
			"id",
			"slot_id",
			"spell_id",
		};
	}

	static std::string ColumnsRaw()
	{
		return std::string(implode(", ", Columns()));
	}

	static std::string SelectColumnsRaw()
	{
		return std::string(implode(", ", SelectColumns()));
	}

	static std::string TableName()
	{
		return std::string("character_spells");
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

	static CharacterSpells NewEntity()
	{
		CharacterSpells entry{};

		entry.id       = 0;
		entry.slot_id  = 0;
		entry.spell_id = 0;

		return entry;
	}

	static CharacterSpells GetCharacterSpellsEntry(
		const std::vector<CharacterSpells> &character_spellss,
		int character_spells_id
	)
	{
		for (auto &character_spells : character_spellss) {
			if (character_spells.id == character_spells_id) {
				return character_spells;
			}
		}

		return NewEntity();
	}

	static CharacterSpells FindOne(
		Database& db,
		int character_spells_id
	)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"{} WHERE id = {} LIMIT 1",
				BaseSelect(),
				character_spells_id
			)
		);

		auto row = results.begin();
		if (results.RowCount() == 1) {
			CharacterSpells entry{};

			entry.id       = atoi(row[0]);
			entry.slot_id  = atoi(row[1]);
			entry.spell_id = atoi(row[2]);

			return entry;
		}

		return NewEntity();
	}

	static int DeleteOne(
		Database& db,
		int character_spells_id
	)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"DELETE FROM {} WHERE {} = {}",
				TableName(),
				PrimaryKey(),
				character_spells_id
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int UpdateOne(
		Database& db,
		CharacterSpells character_spells_entry
	)
	{
		std::vector<std::string> update_values;

		auto columns = Columns();

		update_values.push_back(columns[1] + " = " + std::to_string(character_spells_entry.slot_id));
		update_values.push_back(columns[2] + " = " + std::to_string(character_spells_entry.spell_id));

		auto results = db.QueryDatabase(
			fmt::format(
				"UPDATE {} SET {} WHERE {} = {}",
				TableName(),
				implode(", ", update_values),
				PrimaryKey(),
				character_spells_entry.id
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static CharacterSpells InsertOne(
		Database& db,
		CharacterSpells character_spells_entry
	)
	{
		std::vector<std::string> insert_values;

		insert_values.push_back(std::to_string(character_spells_entry.id));
		insert_values.push_back(std::to_string(character_spells_entry.slot_id));
		insert_values.push_back(std::to_string(character_spells_entry.spell_id));

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES ({})",
				BaseInsert(),
				implode(",", insert_values)
			)
		);

		if (results.Success()) {
			character_spells_entry.id = results.LastInsertedID();
			return character_spells_entry;
		}

		character_spells_entry = NewEntity();

		return character_spells_entry;
	}

	static int InsertMany(
		Database& db,
		std::vector<CharacterSpells> character_spells_entries
	)
	{
		std::vector<std::string> insert_chunks;

		for (auto &character_spells_entry: character_spells_entries) {
			std::vector<std::string> insert_values;

			insert_values.push_back(std::to_string(character_spells_entry.id));
			insert_values.push_back(std::to_string(character_spells_entry.slot_id));
			insert_values.push_back(std::to_string(character_spells_entry.spell_id));

			insert_chunks.push_back("(" + implode(",", insert_values) + ")");
		}

		std::vector<std::string> insert_values;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES {}",
				BaseInsert(),
				implode(",", insert_chunks)
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static std::vector<CharacterSpells> All(Database& db)
	{
		std::vector<CharacterSpells> all_entries;

		auto results = db.QueryDatabase(
			fmt::format(
				"{}",
				BaseSelect()
			)
		);

		all_entries.reserve(results.RowCount());

		for (auto row = results.begin(); row != results.end(); ++row) {
			CharacterSpells entry{};

			entry.id       = atoi(row[0]);
			entry.slot_id  = atoi(row[1]);
			entry.spell_id = atoi(row[2]);

			all_entries.push_back(entry);
		}

		return all_entries;
	}

	static std::vector<CharacterSpells> GetWhere(Database& db, std::string where_filter)
	{
		std::vector<CharacterSpells> all_entries;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} WHERE {}",
				BaseSelect(),
				where_filter
			)
		);

		all_entries.reserve(results.RowCount());

		for (auto row = results.begin(); row != results.end(); ++row) {
			CharacterSpells entry{};

			entry.id       = atoi(row[0]);
			entry.slot_id  = atoi(row[1]);
			entry.spell_id = atoi(row[2]);

			all_entries.push_back(entry);
		}

		return all_entries;
	}

	static int DeleteWhere(Database& db, std::string where_filter)
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

};

#endif //EQEMU_BASE_CHARACTER_SPELLS_REPOSITORY_H
