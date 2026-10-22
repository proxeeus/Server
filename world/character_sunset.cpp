#include "../common/global_define.h"
#include "character_sunset.h"
#include "clientlist.h"
#include "cliententry.h"
#include "zonelist.h"
#include "worlddb.h"
#include "../common/servertalk.h"
#include "../common/rulesys.h"
#include "../common/strings.h"
#include "../common/eq_constants.h"
#include "../common/emu_constants.h"
#include "../common/eqemu_logsys.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

extern ClientList client_list;
extern ZSList     zoneserver_list;

namespace {

struct Target {
	uint32      id{};
	uint32      account_id{};
	std::string name;
	std::string account_name;
};

// A live row.  Trilogy's char select and zone-in also treat a zero date as live.
constexpr const char *kLiveCharacter = "(cd.`deleted_at` IS NULL OR cd.`deleted_at` = '0000-00-00 00:00:00')";

// Trilogy char select: CharacterSelect_Struct has 10 slots and SendCharSelect LIMIT 10s.
constexpr uint32 kCharacterSlots = 10;

constexpr const char *kNoTables = "No sunset tables: run utils/sql/git/optional/2026_10_22_character_sunset.sql.";

std::string Field(const char *buf, size_t size)
{
	return std::string(buf, strnlen(buf, size));
}

void Reply(const std::string &to, const std::string &message)
{
	zoneserver_list.SendEmoteMessageRaw(to.c_str(), 0, AccountStatus::Player, Chat::White, message.c_str());
}

bool Exec(const std::string &query, bool need_row = false)
{
	auto r = database.QueryDatabase(query);
	return r.Success() && (!need_row || r.RowsAffected() > 0);
}

// Same shape as Database::DeleteCharacter: SUBSTRING(CONCAT(name, '-deleted-', UNIX_TIMESTAMP()), 1, 64)
std::string SunsetName(const std::string &name, time_t ts)
{
	auto n = fmt::format("{}-deleted-{}", name, ts);
	if (n.size() > 64) {
		n.resize(64);
	}

	return n;
}

std::set<std::string> ProtectedAccounts()
{
	std::set<std::string> out;
	for (auto s : Strings::Split(RuleS(Character, SunsetProtectedAccounts), ',')) {
		Strings::Trim(s);
		if (!s.empty()) {
			out.insert(Strings::ToLower(s));
		}
	}

	return out;
}

std::string ProtectedList()
{
	const auto p = ProtectedAccounts();
	return p.empty() ? "none" : Strings::Join(std::vector<std::string>(p.begin(), p.end()), ", ");
}

// WHERE clause over `character_data cd LEFT JOIN account a`.  A character whose
// account row is gone has a NULL a.name and is never protected.
std::string TargetWhere(uint8 action, const std::string &target, uint32 requester_char_id)
{
	std::string where = kLiveCharacter;

	if (action == CharacterSunset::Account) {
		where += fmt::format(" AND a.`name` = '{}'", Strings::Escape(target));
	}
	else if (action == CharacterSunset::Character) {
		where += fmt::format(" AND cd.`name` = '{}'", Strings::Escape(target));
	}

	const auto p = ProtectedAccounts();
	if (!p.empty()) {
		std::vector<std::string> quoted;
		for (const auto &s : p) {
			quoted.push_back(fmt::format("'{}'", Strings::Escape(s)));
		}

		where += fmt::format(" AND COALESCE(LOWER(a.`name`), '') NOT IN ({})", Strings::Join(quoted, ","));
	}

	if (requester_char_id) {
		where += fmt::format(" AND cd.`id` != {}", requester_char_id);
	}

	return where;
}

std::vector<Target> LoadTargets(const std::string &where)
{
	std::vector<Target> out;

	auto r = database.QueryDatabase(
		fmt::format(
			"SELECT cd.`id`, cd.`account_id`, cd.`name`, COALESCE(a.`name`, '') "
			"FROM `character_data` cd LEFT JOIN `account` a ON a.`id` = cd.`account_id` "
			"WHERE {} ORDER BY a.`name`, cd.`name`",
			where
		)
	);
	if (!r.Success()) {
		return out;
	}

	for (auto row : r) {
		Target t;
		t.id           = Strings::ToUnsignedInt(row[0]);
		t.account_id   = Strings::ToUnsignedInt(row[1]);
		t.name         = row[2] ? row[2] : "";
		t.account_name = row[3] ? row[3] : "";
		out.push_back(t);
	}

	return out;
}

uint32 CountBots(const std::string &where)
{
	if (!RuleB(Bots, Enabled)) {
		return 0;
	}

	auto r = database.QueryDatabase(
		fmt::format(
			"SELECT COUNT(*) FROM `bot_data` b "
			"JOIN `character_data` cd ON cd.`id` = b.`owner_id` "
			"LEFT JOIN `account` a ON a.`id` = cd.`account_id` "
			"WHERE b.`name` NOT LIKE '%-deleted-%' AND {}",
			where
		)
	);
	if (!r.Success() || !r.RowCount()) {
		return 0;
	}

	auto row = r.begin();
	return Strings::ToUnsignedInt(row[0]);
}

// One line per account: "puty (4): Abc, Def, Ghi".
void ReplyByAccount(const std::string &to, const std::vector<Target> &targets)
{
	constexpr size_t kMaxAccounts = 20;
	constexpr size_t kMaxNames    = 8;

	std::map<std::string, std::vector<std::string>> by_account;
	for (const auto &t : targets) {
		by_account[t.account_name.empty() ? "(no account)" : t.account_name].push_back(t.name);
	}

	size_t shown = 0;
	for (const auto &[account, names] : by_account) {
		if (shown++ == kMaxAccounts) {
			Reply(to, fmt::format("  ... and {} more accounts", by_account.size() - kMaxAccounts));
			break;
		}

		std::vector<std::string> head(names.begin(), names.begin() + std::min(names.size(), kMaxNames));
		Reply(
			to,
			fmt::format(
				"  {} ({}): {}{}",
				account,
				names.size(),
				Strings::Join(head, ", "),
				names.size() > kMaxNames ? fmt::format(", +{} more", names.size() - kMaxNames) : ""
			)
		);
	}
}

// Any CLE for this character that is not at char select or offline means a
// zone may still hold its Client.  Duplicate CLEs exist on the Trilogy path
// (one from LS auth, one from the zone's UpdateWho), so look it up both ways.
bool MaybeInZone(const Target &t)
{
	for (auto cle : {client_list.FindCLEByCharacterID(t.id), client_list.FindCharacter(t.name.c_str())}) {
		if (cle && cle->Online() >= CLE_Status::Zoning) {
			return true;
		}
	}

	return false;
}

// By the old (in-memory) name.  Zones that do not hold the character ignore it.
void Kick(const Target &t, const ServerCharacterSunset_Struct *req)
{
	auto pack = new ServerPacket(ServerOP_KickPlayer, sizeof(ServerKickPlayer_Struct));
	auto skp  = reinterpret_cast<ServerKickPlayer_Struct *>(pack->pBuffer);
	strn0cpy(skp->adminname, req->requester, sizeof(skp->adminname));
	strn0cpy(skp->name, t.name.c_str(), sizeof(skp->name));
	skp->adminrank = req->requester_admin;
	zoneserver_list.SendPacket(pack);
	safe_delete(pack);
}

void DoPreview(const ServerCharacterSunset_Struct *req, const std::string &to)
{
	const auto where   = TargetWhere(CharacterSunset::All, "", req->requester_char_id);
	const auto targets = LoadTargets(where);

	std::set<uint32> accounts;
	for (const auto &t : targets) {
		accounts.insert(t.account_id);
	}

	Reply(
		to,
		fmt::format(
			"Sunset preview: {} characters on {} accounts, {} bots. Protected accounts: {}.",
			targets.size(),
			accounts.size(),
			CountBots(where),
			ProtectedList()
		)
	);

	ReplyByAccount(to, targets);

	if (!targets.empty()) {
		Reply(to, "#sunset all to sunset them. Every batch can be put back with #sunset undo.");
	}
}

void DoSunset(const ServerCharacterSunset_Struct *req, const std::string &to, const std::string &target)
{
	const uint8 action = req->action;

	if (action == CharacterSunset::Account && ProtectedAccounts().count(Strings::ToLower(target))) {
		Reply(to, fmt::format("Account [{}] is protected (Character:SunsetProtectedAccounts).", target));
		return;
	}

	const auto targets = LoadTargets(TargetWhere(action, target, req->requester_char_id));
	if (targets.empty()) {
		Reply(
			to,
			action == CharacterSunset::All ?
			"Nothing to sunset." :
			fmt::format(
				"Nothing to sunset for [{}]: unknown, already sunset, on a protected account ({}), or yourself.",
				target,
				ProtectedList()
			)
		);
		return;
	}

	const std::string scope =
		action == CharacterSunset::All ? "all" :
		action == CharacterSunset::Account ? fmt::format("account {}", target) :
		fmt::format("character {}", target);

	auto br = database.QueryDatabase(
		fmt::format(
			"INSERT INTO `character_sunset_batches` (`actor`, `scope`) VALUES ('{}', '{}')",
			Strings::Escape(to),
			Strings::Escape(scope)
		)
	);
	if (!br.Success() || !br.LastInsertedID()) {
		Reply(to, kNoTables);
		return;
	}

	const uint32 batch_id   = br.LastInsertedID();
	const time_t ts         = std::time(nullptr);
	uint32       characters = 0;
	uint32       bots       = 0;
	std::vector<std::string> failed;
	std::vector<std::string> kicked;

	for (const auto &t : targets) {
		const auto new_name = SunsetName(t.name, ts);
		uint32     t_bots   = 0;

		database.TransactionBegin();

		bool ok = Exec(
			fmt::format(
				"UPDATE `character_data` cd SET cd.`name` = '{}', cd.`deleted_at` = NOW() WHERE cd.`id` = {} AND {}",
				Strings::Escape(new_name),
				t.id,
				kLiveCharacter
			),
			true
		) && Exec(
			fmt::format(
				"INSERT INTO `character_sunset_entries` "
				"(`batch_id`, `kind`, `entity_id`, `character_id`, `account_id`, `old_name`, `new_name`) "
				"VALUES ({}, 0, {}, {}, {}, '{}', '{}')",
				batch_id,
				t.id,
				t.id,
				t.account_id,
				Strings::Escape(t.name),
				Strings::Escape(new_name)
			)
		);

		if (ok && RuleB(Bots, Enabled)) {
			auto bot_rows = database.QueryDatabase(
				fmt::format(
					"SELECT `bot_id`, `name` FROM `bot_data` WHERE `owner_id` = {} AND `name` NOT LIKE '%-deleted-%'",
					t.id
				)
			);
			ok = bot_rows.Success();

			for (auto row : bot_rows) {
				if (!ok) {
					break;
				}

				const uint32 bot_id       = Strings::ToUnsignedInt(row[0]);
				const std::string old_bot = row[1] ? row[1] : "";
				const std::string new_bot = SunsetName(old_bot, ts);

				ok = Exec(
					fmt::format(
						"UPDATE `bot_data` SET `name` = '{}' WHERE `bot_id` = {}",
						Strings::Escape(new_bot),
						bot_id
					),
					true
				) && Exec(
					fmt::format(
						"INSERT INTO `character_sunset_entries` "
						"(`batch_id`, `kind`, `entity_id`, `character_id`, `account_id`, `old_name`, `new_name`) "
						"VALUES ({}, 1, {}, {}, {}, '{}', '{}')",
						batch_id,
						bot_id,
						t.id,
						t.account_id,
						Strings::Escape(old_bot),
						Strings::Escape(new_bot)
					)
				);

				if (ok) {
					++t_bots;
				}
			}
		}

		if (!ok) {
			database.TransactionRollback();
			failed.push_back(t.name);
			LogInfo("[Sunset] batch [{}] FAILED character [{}] ID [{}], rolled back", batch_id, t.name, t.id);
			continue;
		}

		database.TransactionCommit();
		++characters;
		bots += t_bots;

		LogInfo(
			"[Sunset] batch [{}] character [{}] ID [{}] account [{}] -> [{}], bots [{}]",
			batch_id,
			t.name,
			t.id,
			t.account_name,
			new_name,
			t_bots
		);

		if (MaybeInZone(t)) {
			Kick(t, req);
			kicked.push_back(t.name);
		}
	}

	if (characters) {
		Exec(
			fmt::format(
				"UPDATE `character_sunset_batches` SET `characters` = {}, `bots` = {} WHERE `id` = {}",
				characters,
				bots,
				batch_id
			)
		);
	}
	else {
		Exec(fmt::format("DELETE FROM `character_sunset_batches` WHERE `id` = {}", batch_id));
	}

	Reply(
		to,
		characters ?
		fmt::format("Sunset batch #{} ({}): {} characters, {} bots. Undo with #sunset undo {}.", batch_id, scope, characters, bots, batch_id) :
		fmt::format("Sunset ({}) changed nothing.", scope)
	);

	if (!kicked.empty()) {
		Reply(to, fmt::format("Kicked (were online): {}", Strings::Join(kicked, ", ")));
	}

	if (!failed.empty()) {
		Reply(to, fmt::format("Failed and left untouched (see world log [Sunset]): {}", Strings::Join(failed, ", ")));
	}
}

void DoListBatches(const std::string &to)
{
	auto r = database.QueryDatabase(
		"SELECT `id`, `created_at`, `actor`, `scope`, `characters`, `bots`, COALESCE(`restored_at`, '') "
		"FROM `character_sunset_batches` ORDER BY `id` DESC LIMIT 10"
	);
	if (!r.Success()) {
		Reply(to, kNoTables);
		return;
	}

	if (!r.RowCount()) {
		Reply(to, "No sunset batches yet.");
		return;
	}

	Reply(to, "Recent sunset batches:");
	for (auto row : r) {
		const std::string restored = row[6] ? row[6] : "";
		Reply(
			to,
			fmt::format(
				"  #{} {} by {} | {} | {} characters, {} bots{}",
				row[0],
				row[1],
				row[2],
				row[3],
				row[4],
				row[5],
				restored.empty() ? "" : fmt::format(" | restored {}", restored)
			)
		);
	}
}

// Live characters on the account that owns character_id.
uint32 LiveCharactersOnAccountOf(uint32 character_id)
{
	auto r = database.QueryDatabase(
		fmt::format(
			"SELECT COUNT(*) FROM `character_data` cd WHERE {} AND cd.`account_id` = "
			"(SELECT `account_id` FROM `character_data` WHERE `id` = {})",
			kLiveCharacter,
			character_id
		)
	);
	if (!r.Success() || !r.RowCount()) {
		return 0;
	}

	auto row = r.begin();
	return Strings::ToUnsignedInt(row[0]);
}

// target = an old character name: restore just that character (and its bots).
// With no batch number, the most recent batch that still holds it is used.
void DoUndo(const ServerCharacterSunset_Struct *req, const std::string &to, const std::string &target)
{
	uint32 batch_id = req->batch_id;
	if (!batch_id && !target.empty()) {
		auto r = database.QueryDatabase(
			fmt::format(
				"SELECT COALESCE(MAX(`batch_id`), 0) FROM `character_sunset_entries` "
				"WHERE `kind` = 0 AND `restored` = 0 AND `old_name` = '{}'",
				Strings::Escape(target)
			)
		);
		if (r.Success() && r.RowCount()) {
			auto row = r.begin();
			batch_id = Strings::ToUnsignedInt(row[0]);
		}

		if (!batch_id) {
			Reply(to, fmt::format("No sunset character named [{}] left to restore.", target));
			return;
		}
	}

	if (!batch_id) {
		auto r = database.QueryDatabase(
			"SELECT COALESCE(MAX(`id`), 0) FROM `character_sunset_batches` WHERE `restored_at` IS NULL"
		);
		if (r.Success() && r.RowCount()) {
			auto row = r.begin();
			batch_id = Strings::ToUnsignedInt(row[0]);
		}
	}

	if (!batch_id) {
		Reply(to, "No sunset batch to undo.");
		return;
	}

	struct Entry {
		uint8       kind{};
		uint32      entity_id{};
		std::string old_name;
		std::string new_name;
	};

	// character id -> [character entry, bot entries...]
	std::map<uint32, std::vector<Entry>> by_character;

	auto r = database.QueryDatabase(
		fmt::format(
			"SELECT `kind`, `entity_id`, `character_id`, `old_name`, `new_name` FROM `character_sunset_entries` "
			"WHERE `batch_id` = {} AND `restored` = 0{} ORDER BY `kind`, `entity_id`",
			batch_id,
			target.empty() ?
			"" :
			fmt::format(
				" AND `character_id` IN (SELECT `character_id` FROM `character_sunset_entries` "
				"WHERE `batch_id` = {} AND `kind` = 0 AND `old_name` = '{}')",
				batch_id,
				Strings::Escape(target)
			)
		)
	);
	if (!r.Success()) {
		Reply(to, kNoTables);
		return;
	}

	for (auto row : r) {
		Entry e;
		e.kind      = static_cast<uint8>(Strings::ToUnsignedInt(row[0]));
		e.entity_id = Strings::ToUnsignedInt(row[1]);
		e.old_name  = row[3] ? row[3] : "";
		e.new_name  = row[4] ? row[4] : "";
		by_character[Strings::ToUnsignedInt(row[2])].push_back(e);
	}

	if (by_character.empty()) {
		Reply(
			to,
			target.empty() ?
			fmt::format("Sunset batch #{} has nothing left to restore.", batch_id) :
			fmt::format("Sunset batch #{} holds no [{}] left to restore.", batch_id, target)
		);
		return;
	}

	uint32 characters = 0;
	uint32 bots       = 0;
	std::vector<std::string> skipped;

	for (const auto &[character_id, entries] : by_character) {
		database.TransactionBegin();

		// No character entry left means the owner came back on an earlier undo and
		// only bots whose names were taken remain; retry just those.
		const bool owner_pending = std::any_of(
			entries.begin(),
			entries.end(),
			[](const Entry &e) { return e.kind == 0; }
		);

		bool character_back = !owner_pending;
		for (const auto &e : entries) {
			if (e.kind != 0) {
				continue;
			}

			// The old name may have been taken since (a new test character, say).
			if (database.IsNameUsed(e.old_name)) {
				skipped.push_back(fmt::format("{} (name taken)", e.old_name));
				break;
			}

			// A character past the last slot exists but never shows at char select.
			if (LiveCharactersOnAccountOf(e.entity_id) >= kCharacterSlots) {
				skipped.push_back(fmt::format("{} (account full)", e.old_name));
				break;
			}

			// Matching on new_name means a row changed since the sunset is left alone.
			character_back = Exec(
				fmt::format(
					"UPDATE `character_data` SET `name` = '{}', `deleted_at` = NULL WHERE `id` = {} AND `name` = '{}'",
					Strings::Escape(e.old_name),
					e.entity_id,
					Strings::Escape(e.new_name)
				),
				true
			) && Exec(
				fmt::format(
					"UPDATE `character_sunset_entries` SET `restored` = 1 WHERE `batch_id` = {} AND `kind` = 0 AND `entity_id` = {}",
					batch_id,
					e.entity_id
				)
			);

			if (!character_back) {
				skipped.push_back(fmt::format("{} (changed since)", e.old_name));
			}
		}

		if (!character_back) {
			database.TransactionRollback();
			continue;
		}

		for (const auto &e : entries) {
			if (e.kind != 1) {
				continue;
			}

			// A bot whose name was taken stays sunset; its owner still comes back.
			if (database.IsNameUsed(e.old_name)) {
				skipped.push_back(fmt::format("bot {} (name taken)", e.old_name));
				continue;
			}

			if (
				Exec(
					fmt::format(
						"UPDATE `bot_data` SET `name` = '{}' WHERE `bot_id` = {} AND `name` = '{}'",
						Strings::Escape(e.old_name),
						e.entity_id,
						Strings::Escape(e.new_name)
					),
					true
				) &&
				Exec(
					fmt::format(
						"UPDATE `character_sunset_entries` SET `restored` = 1 WHERE `batch_id` = {} AND `kind` = 1 AND `entity_id` = {}",
						batch_id,
						e.entity_id
					)
				)
			) {
				++bots;
			}
			else {
				skipped.push_back(fmt::format("bot {} (changed since)", e.old_name));
			}
		}

		database.TransactionCommit();
		if (owner_pending) {
			++characters;
			LogInfo("[Sunset] batch [{}] restored character ID [{}]", batch_id, character_id);
		}
	}

	// The batch counts as restored once no character entry is left sunset.
	Exec(
		fmt::format(
			"UPDATE `character_sunset_batches` SET `restored_at` = NOW() WHERE `id` = {} AND NOT EXISTS "
			"(SELECT 1 FROM `character_sunset_entries` WHERE `batch_id` = {} AND `kind` = 0 AND `restored` = 0)",
			batch_id,
			batch_id
		)
	);

	Reply(to, fmt::format("Sunset batch #{} restored: {} characters, {} bots.", batch_id, characters, bots));

	if (!skipped.empty()) {
		Reply(to, fmt::format("Skipped: {}", Strings::Join(skipped, ", ")));
	}
}

} // namespace

void CharacterSunset::HandleRequest(const ServerCharacterSunset_Struct *req)
{
	const auto to     = Field(req->requester, sizeof(req->requester));
	const auto target = Field(req->target, sizeof(req->target));

	if (req->requester_admin < AccountStatus::GMImpossible) {
		LogInfo("[Sunset] refused request from [{}] status [{}]", to, req->requester_admin);
		return;
	}

	LogInfo("[Sunset] request from [{}] action [{}] target [{}] batch [{}]", to, req->action, target, req->batch_id);

	switch (req->action) {
		case Preview:
			DoPreview(req, to);
			break;
		case All:
		case Account:
		case Character:
			DoSunset(req, to, target);
			break;
		case Batches:
			DoListBatches(to);
			break;
		case Undo:
			DoUndo(req, to, target);
			break;
		default:
			break;
	}
}
