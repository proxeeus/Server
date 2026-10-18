#include "../client.h"
#include "../entity.h"
#include "../npc.h"
#include "../playerbot_native.h"

void command_pbnative(Client *c, const Seperator *sep)
{
	const std::string sub = sep->arg[1] ? Strings::ToLower(sep->arg[1]) : "";

	if (sub == "reload") {
		std::string summary;
		if (playerbot_native.Reload(summary)) {
			c->Message(Chat::White, "%s", fmt::format("[pbnative] reloaded: {}", summary).c_str());
		}
		else {
			c->Message(Chat::Red, "%s", fmt::format("[pbnative] reload FAILED (new PlayerBots spawn bare until it succeeds): {}", summary).c_str());
		}
		return;
	}

	if (sub == "status") {
		c->Message(Chat::White, "%s", fmt::format("[pbnative] {}", playerbot_native.Status()).c_str());
		return;
	}

	if (sub == "info") {
		Mob *t = c->GetTarget();
		if (!PlayerBotNative::IsPlayerBot(t)) {
			c->Message(Chat::White, "[pbnative] target a PlayerBot.");
			return;
		}

		NPC *npc = t->CastToNPC();

		c->Message(
			Chat::White, "%s",
			fmt::format(
				"[pbnative] [{}] | level [{}] class [{}] race [{}] gender [{}] face [{}] size [{:.1f}] | "
				"loot [{}] | hp [{}/{}] mana [{}/{}] | spells_id [{}] | roambox [{}]",
				npc->playerbot_temp_name,
				npc->GetLevel(), npc->GetClass(), npc->GetRace(), npc->GetGender(), npc->GetLuclinFace(), npc->GetSize(),
				npc->GetLootItems().size(),
				npc->GetHP(), npc->GetMaxHP(), npc->GetMana(), npc->GetMaxMana(),
				npc->GetNPCSpellsID(),
				npc->HasRoamBox() ? "yes" : "no"
			).c_str()
		);
		return;
	}

	c->Message(Chat::White, "usage:");
	c->Message(Chat::White, "  #pbnative status - content load state and this zone's generated/failed counts");
	c->Message(Chat::White, "  #pbnative reload - re-read the playerbot_* content tables (names included)");
	c->Message(Chat::White, "  #pbnative info - on a targeted PlayerBot: what it rolled");
}
