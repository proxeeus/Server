#include "../client.h"
#include "../worldserver.h"

extern WorldServer worldserver;

// #sunset - soft-delete characters in bulk (world/character_sunset.cpp does the
// work; it is the only process that knows who is online and must be kicked).
void command_sunset(Client *c, const Seperator *sep)
{
	const std::string sub = sep->argnum ? Strings::ToLower(sep->arg[1]) : "preview";

	uint8       action   = CharacterSunset::Preview;
	uint32      batch_id = 0;
	std::string target;

	if (sub == "preview") {
		action = CharacterSunset::Preview;
	}
	else if (sub == "all") {
		action = CharacterSunset::All;
	}
	else if ((sub == "account" || sub == "char" || sub == "character") && sep->argnum >= 2) {
		action = sub == "account" ? CharacterSunset::Account : CharacterSunset::Character;
		target = sep->arg[2];
	}
	else if (sub == "batches" || sub == "list") {
		action = CharacterSunset::Batches;
	}
	else if (sub == "undo") {
		action   = CharacterSunset::Undo;
		batch_id = sep->argnum >= 2 ? Strings::ToUnsignedInt(sep->arg[2]) : 0;
	}
	else {
		c->Message(Chat::White, "Usage: #sunset [preview] - who #sunset all would sunset");
		c->Message(Chat::White, "Usage: #sunset all - soft-delete every character outside Character:SunsetProtectedAccounts");
		c->Message(Chat::White, "Usage: #sunset account [Account Name] | #sunset char [Character Name]");
		c->Message(Chat::White, "Usage: #sunset batches - recent batches | #sunset undo [Batch #] - restore one (default: latest)");
		return;
	}

	if (!worldserver.Connected()) {
		c->Message(Chat::White, "The world server is currently disconnected.");
		return;
	}

	auto pack = new ServerPacket(ServerOP_CharacterSunset, sizeof(ServerCharacterSunset_Struct));
	auto s    = reinterpret_cast<ServerCharacterSunset_Struct *>(pack->pBuffer);
	strn0cpy(s->requester, c->GetName(), sizeof(s->requester));
	s->requester_char_id = c->CharacterID();
	s->requester_admin   = c->Admin();
	s->action            = action;
	s->batch_id          = batch_id;
	strn0cpy(s->target, target.c_str(), sizeof(s->target));
	worldserver.SendPacket(pack);
	safe_delete(pack);
}
