#ifndef WORLD_CHARACTER_SUNSET_H
#define WORLD_CHARACTER_SUNSET_H

#include "../common/servertalk.h"

// ============================================================
// #sunset - soft-delete characters in bulk, restorably.
//
// Each character gets exactly what a char-select delete does under
// Character:SoftDeletes (Database::DeleteCharacter): the name becomes
// "<name>-deleted-<unix ts>" so it can be reused, deleted_at = NOW(), and the
// character's bots get the same rename.  Nothing is hard-deleted.  Every rename
// is recorded in character_sunset_batches / character_sunset_entries
// (utils/sql/git/optional/2026_10_22_character_sunset.sql, run by hand) so
// "#sunset undo" can put a batch, or one character, back.  Undo skips a
// character whose name is taken or whose account already has 10 live
// characters (Trilogy char select shows 10); it stays restorable.
//
// Accounts named in Character:SunsetProtectedAccounts are never touched, nor is
// the requesting character.  Characters that are online are sunset anyway and
// kicked; ZoneDatabase::SaveCharacterData and BotDatabase::SaveBot refuse to
// write the old name back over a sunset row on their way out.
// ============================================================

namespace CharacterSunset {
	// Action lives with ServerCharacterSunset_Struct in common/servertalk.h.
	void HandleRequest(const ServerCharacterSunset_Struct *req);
}

#endif
