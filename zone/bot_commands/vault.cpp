#include "../bot_command.h"

// Bot vault: bot-bound gear the bot is not wearing.  See
// utils/sql/git/bots/optional/2026_10_20_bots_bot_vault.sql for the rules.

void bot_command_vault_list(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_vault_list", sep->arg[0], "vaultlist")) {
		return;
	}

	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(
			Chat::White,
			fmt::format(
				"Usage: {} ([actionable: target | byname] ([actionable_name]))",
				sep->arg[0]
			).c_str()
		);
		return;
	}

	int ab_mask = (ActionableBots::ABM_Target | ActionableBots::ABM_ByName);

	std::list<Bot*> sbl;
	if (ActionableBots::PopulateSBL(c, sep->arg[1], sbl, ab_mask, sep->arg[2]) == ActionableBots::ABT_None) {
		return;
	}

	auto my_bot = sbl.empty() ? nullptr : sbl.front();
	if (!my_bot) {
		c->Message(Chat::White, "ActionableBots returned 'nullptr'");
		return;
	}

	std::vector<BotVaultItem> items;
	if (!database.botdb.LoadVaultItems(my_bot->GetBotID(), items)) {
		c->Message(Chat::Yellow, "The bot vault could not be read (is 2026_10_20_bots_bot_vault.sql applied?).");
		return;
	}

	if (items.empty()) {
		c->Message(Chat::White, fmt::format("{}'s vault is empty.", my_bot->GetCleanName()).c_str());
		return;
	}

	c->Message(
		Chat::White,
		fmt::format(
			"{}'s vault ({} item{}):",
			my_bot->GetCleanName(),
			items.size(),
			items.size() != 1 ? "s" : ""
		).c_str()
	);

	for (const auto& e : items) {
		const auto* item = database.GetItem(e.item_id);
		if (!item) {
			c->Message(Chat::White, fmt::format("  #{} | unknown item {}", e.vault_id, e.item_id).c_str());
			continue;
		}

		std::vector<std::string> fits;
		for (int16 s = EQ::invslot::EQUIPMENT_BEGIN; s <= EQ::invslot::EQUIPMENT_END; ++s) {
			if (item->Slots & (1 << s)) {
				fits.emplace_back(EQ::invslot::GetInvPossessionsSlotName(s));
			}
		}

		// Trilogy v29c can't parse SayLink tags.
		const std::string equip_link = c->IsTrilogyClient()
			? ""
			: fmt::format(" | {}", Saylink::Silent(fmt::format("^vaultequip {}", e.vault_id), "Equip"));

		c->Message(
			Chat::White,
			fmt::format(
				"  #{} | {} | {}{}",
				e.vault_id,
				item->Name,
				fits.empty() ? "no slot" : Strings::Join(fits, ", "),
				equip_link
			).c_str()
		);
	}

	c->Message(Chat::White, "Use ^vaultequip <#> [slot_id] to put one on.");
}

void bot_command_vault_equip(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_vault_equip", sep->arg[0], "vaultequip")) {
		return;
	}

	if (helper_is_help_or_usage(sep->arg[1]) || !sep->IsNumber(1)) {
		c->Message(
			Chat::White,
			fmt::format(
				"Usage: {} [vault #] ([slot_id: 0-22]) ([actionable: target | byname] ([actionable_name]))",
				sep->arg[0]
			).c_str()
		);
		c->Message(Chat::White, "  vault # comes from ^vaultlist.  slot_id forces the equipment slot; omit it to let the bot choose.");
		return;
	}

	const uint32 vault_id = Strings::ToUnsignedInt(sep->arg[1]);

	// Optional slot_id at arg[2], as ^inventorygive.
	int16 forced_slot        = -1;
	int   actionable_arg_idx = 2;
	if (sep->IsNumber(2)) {
		const int parsed = Strings::ToInt(sep->arg[2]);
		if (parsed < EQ::invslot::EQUIPMENT_BEGIN || parsed > EQ::invslot::EQUIPMENT_END) {
			c->Message(
				Chat::Yellow,
				fmt::format(
					"Invalid slot_id {}; valid equipment slots are {} to {}.",
					parsed,
					(int)EQ::invslot::EQUIPMENT_BEGIN,
					(int)EQ::invslot::EQUIPMENT_END
				).c_str()
			);
			return;
		}

		forced_slot        = static_cast<int16>(parsed);
		actionable_arg_idx = 3;
	}

	int ab_mask = (ActionableBots::ABM_Target | ActionableBots::ABM_ByName);

	std::list<Bot*> sbl;
	if (ActionableBots::PopulateSBL(
		c,
		sep->arg[actionable_arg_idx],
		sbl,
		ab_mask,
		sep->arg[actionable_arg_idx + 1]
	) == ActionableBots::ABT_None) {
		return;
	}

	auto my_bot = sbl.empty() ? nullptr : sbl.front();
	if (!my_bot) {
		c->Message(Chat::White, "ActionableBots returned 'nullptr'");
		return;
	}

	my_bot->EquipFromVault(c, vault_id, forced_slot);
}
