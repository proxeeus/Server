#include "../bot_command.h"

void bot_command_stats(Client* bot_owner, const Seperator* sep)
{
	if (!bot_owner->GetTarget())
	{
		bot_owner->Message(Chat::White, "You need a valid target.");
		return;
	}

	//bot_owner->Message(15, "%s has been updated.", bot_owner->GetTarget()->GetCleanName());
	//bot_owner->Message(15, "Level: %i HP: %i AC: %i Mana: %i STR: %i STA: %i DEX: %i AGI: %i INT: %i WIS: %i CHA: %i", bot_owner->GetTarget()->GetLevel(), bot_owner->GetTarget()->GetMaxHP(), bot_owner->GetTarget()->GetAC(), bot_owner->GetTarget()->GetMaxMana(), bot_owner->GetTarget()->GetSTR(), bot_owner->GetTarget()->GetSTA(), bot_owner->GetTarget()->GetDEX(), bot_owner->GetTarget()->GetAGI(), bot_owner->GetTarget()->GetINT(), bot_owner->GetTarget()->GetWIS(), bot_owner->GetTarget()->GetCHA());
	//bot_owner->Message(15, "Resists-- Magic: %i, Poison: %i, Fire: %i, Cold: %i, Disease: %i, Corruption: %i.", bot_owner->GetTarget()->GetMR(), bot_owner->GetTarget()->GetPR(), bot_owner->GetTarget()->GetFR(), bot_owner->GetTarget()->GetCR(), bot_owner->GetTarget()->GetDR(), bot_owner->GetTarget()->GetCorrup());
	bot_owner->GetTarget()->CastToBot()->CalcBotStats(true);
}
