#include "../../bot.h"
#include "../../client.h"

void SetLevel(Client *c, const Seperator *sep)
{
	const auto arguments = sep->argnum;
	if (arguments < 2 || !sep->IsNumber(2)) {
		c->Message(Chat::White, "Usage: #set level [Level]");
		return;
	}

	Mob* t = c;
	if (c->GetTarget()) {
		t = c->GetTarget();
	}

	const uint8 max_level = RuleI(Character, MaxLevel);
	const uint8 level     = Strings::ToUnsignedInt(sep->arg[2]);

	if (c != t && c->Admin() < RuleI(GM, MinStatusToLevelTarget)) {
		c->Message(Chat::White, "Your status is not high enough to change another person's level.");
		return;
	}

	const uint8  old_level  = t->GetLevel();
	const uint16 old_points = t->IsClient() ? t->CastToClient()->GetSkillPoints() : 0;

	t->SetLevel(level, true);

	if (t->IsClient()) {
		// Client::SetLevel only grants training points when set_level > m_pp.level2
		// (the highest level ever reached), which is a legitimate anti-farm guard
		// during organic play — but blocks a GM relevel from restoring points after
		// a delevel.  The v29c Trilogy client tracks its own local training-points
		// counter and bumps it per OP_LevelUpdate, so after a GM delevel+relevel
		// it displays (level * 5) practice points while the server still says 0,
		// and the trainer window then rejects every click ("You have no skill
		// points to spend") — appearing to freeze the UI (tier bar and per-skill
		// cost stop advancing) because no OP_SkillUpdate flows back.
		//
		// When the GM levels a character UP via #set level, top up server-side
		// points to match the delta the client just credited itself.  Non-Trilogy
		// clients that read points authoritatively from the server also benefit
		// (relevel testing now grants a consistent 5-per-level).  Level-DOWN and
		// same-level calls make no adjustment.
		if (level > old_level) {
			const uint16 expected_add = static_cast<uint16>(5 * (level - old_level));
			const uint16 new_points   = t->CastToClient()->GetSkillPoints();
			const uint16 actual_add   = new_points - old_points;
			if (actual_add < expected_add) {
				t->CastToClient()->SetSkillPoints(old_points + expected_add);
			}
		}

		for (const auto& s : EQ::skills::GetSkillTypeMap()) {
			const uint16 max_skill_value = t->CastToClient()->MaxSkill(s.first);
			if (t->GetSkill(s.first) > max_skill_value) {
				t->CastToClient()->SetSkill(s.first, max_skill_value);
			}
		}

		t->CastToClient()->SendLevelAppearance();

		if (RuleB(Bots, Enabled) && RuleB(Bots, BotLevelsWithOwner)) {
			Bot::LevelBotWithClient(t->CastToClient(), level, true);
		}
	}
}
