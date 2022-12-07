#ifndef EQEMU_LUA_CLIENT_H
#define EQEMU_LUA_CLIENT_H
#ifdef LUA_EQEMU

#include "lua_mob.h"

class Client;
class Lua_Expedition;
class Lua_Group;
class Lua_Raid;
class Lua_Inventory;
class Lua_Packet;

namespace luabind {
	struct scope;
}

luabind::scope lua_register_client();
luabind::scope lua_register_inventory_where();

class Lua_Client : public Lua_Mob
{
	typedef Client NativeType;
public:
	Lua_Client() { SetLuaPtrData(nullptr); }
	Lua_Client(Client *d) { SetLuaPtrData(reinterpret_cast<Entity*>(d)); }
	virtual ~Lua_Client() { }

	operator Client*() {
		return reinterpret_cast<Client*>(GetLuaPtrData());
	}

	void SendSound();
	void Sit();
	void Save();
	void Save(int commit_now);
	void SaveBackup();
	bool Connected();
	bool InZone();
	void Kick();
	void Disconnect();
	bool IsLD();
	void WorldKick();
	void SendToGuildHall();
	void SendToInstance(std::string instance_type, std::string zone_short_name, uint32 instance_version, float x, float y, float z, float heading, std::string instance_identifier, uint32 duration);
	int GetAnon();
	void SetAnon(uint8 anon_flag);
	int GetAFK();
	void SetAFK(uint8 afk_flag);
	void Duck();
	void DyeArmorBySlot(uint8 slot, uint8 red, uint8 green, uint8 blue);
	void DyeArmorBySlot(uint8 slot, uint8 red, uint8 green, uint8 blue, uint8 use_tint);
	void Stand();
	void SetGM(bool v);
	void SetPVP(bool v);
	bool GetPVP();
	bool GetGM();
	void SetBaseClass(int v);
	void SetBaseRace(int v);
	void SetBaseGender(int v);
	int GetClassBitmask();
	int GetRaceBitmask();
	int GetBaseFace();
	int GetLanguageSkill(int skill_id);
	int GetLDoNPointsTheme(int theme);
	int GetBaseSTR();
	int GetBaseSTA();
	int GetBaseCHA();
	int GetBaseDEX();
	int GetBaseINT();
	int GetBaseAGI();
	int GetBaseWIS();
	int GetWeight();
	uint32 GetEXP();
	double GetEXPModifier(uint32 zone_id);
	double GetEXPModifier(uint32 zone_id, int16 instance_version);
	double GetAAEXPModifier(uint32 zone_id);
	double GetAAEXPModifier(uint32 zone_id, int16 instance_version);
	void SetAAEXPModifier(uint32 zone_id, double aa_modifier);
	void SetAAEXPModifier(uint32 zone_id, double aa_modifier, int16 instance_version);
	void SetEXPModifier(uint32 zone_id, double exp_modifier);
	void SetEXPModifier(uint32 zone_id, double exp_modifier, int16 instance_version);
	uint32 GetAAExp();
	uint32 GetAAPercent();
	uint32 GetTotalSecondsPlayed();
	void AddLDoNLoss(uint32 theme_id);
	void AddLDoNWin(uint32 theme_id);
	void RemoveLDoNLoss(uint32 theme_id);
	void RemoveLDoNWin(uint32 theme_id);
	void UpdateLDoNPoints(uint32 theme_id, int points);
	void SetDeity(int v);
	void AddEXP(uint32 add_exp);
	void AddEXP(uint32 add_exp, int conlevel);
	void AddEXP(uint32 add_exp, int conlevel, bool resexp);
	void SetEXP(uint32 set_exp, uint32 set_aaxp);
	void SetEXP(uint32 set_exp, uint32 set_aaxp, bool resexp);
	void SetBindPoint();
	void SetBindPoint(int to_zone);
	void SetBindPoint(int to_zone, int to_instance);
	void SetBindPoint(int to_zone, int to_instance, float new_x);
	void SetBindPoint(int to_zone, int to_instance, float new_x, float new_y);
	void SetBindPoint(int to_zone, int to_instance, float new_x, float new_y, float new_z);
	void SetBindPoint(int to_zone, int to_instance, float new_x, float new_y, float new_z, float new_heading);
	float GetBindX();
	float GetBindX(int index);
	float GetBindY();
	float GetBindY(int index);
	float GetBindZ();
	float GetBindZ(int index);
	float GetBindHeading();
	float GetBindHeading(int index);
	uint32 GetBindZoneID();
	uint32 GetBindZoneID(int index);
	float GetTargetRingX();
	float GetTargetRingY();
	float GetTargetRingZ();
	void MovePC(int zone, float x, float y, float z, float heading);
	void MovePCInstance(int zone, int instance, float x, float y, float z, float heading);
	void MoveZone(const char *zone_short_name);
	void MoveZone(const char *zone_short_name, float x, float y, float z);
	void MoveZone(const char *zone_short_name, float x, float y, float z, float heading);
	void MoveZoneGroup(const char *zone_short_name);
	void MoveZoneGroup(const char *zone_short_name, float x, float y, float z);
	void MoveZoneGroup(const char *zone_short_name, float x, float y, float z, float heading);
	void MoveZoneRaid(const char *zone_short_name);
	void MoveZoneRaid(const char *zone_short_name, float x, float y, float z);
	void MoveZoneRaid(const char *zone_short_name, float x, float y, float z, float heading);
	void MoveZoneInstance(uint16 instance_id);
	void MoveZoneInstance(uint16 instance_id, float x, float y, float z);
	void MoveZoneInstance(uint16 instance_id, float x, float y, float z, float heading);
	void MoveZoneInstanceGroup(uint16 instance_id);
	void MoveZoneInstanceGroup(uint16 instance_id, float x, float y, float z);
	void MoveZoneInstanceGroup(uint16 instance_id, float x, float y, float z, float heading);
	void MoveZoneInstanceRaid(uint16 instance_id);
	void MoveZoneInstanceRaid(uint16 instance_id, float x, float y, float z);
	void MoveZoneInstanceRaid(uint16 instance_id, float x, float y, float z, float heading);
	bool TeleportToPlayerByCharID(uint32 character_id);
	bool TeleportToPlayerByName(std::string player_name);
	bool TeleportGroupToPlayerByCharID(uint32 character_id);
	bool TeleportGroupToPlayerByName(std::string player_name);
	bool TeleportRaidToPlayerByCharID(uint32 character_id);
	bool TeleportRaidToPlayerByName(std::string player_name);
	void ChangeLastName(std::string last_name);
	int GetFactionLevel(uint32 char_id, uint32 npc_id, uint32 race, uint32 class_, uint32 deity, uint32 faction, Lua_NPC npc);
	void SetFactionLevel(uint32 char_id, uint32 npc_id, int char_class, int char_race, int char_deity);
	void SetFactionLevel2(uint32 char_id, int faction_id, int char_class, int char_race, int char_deity, int value, int temp);
	void RewardFaction(int id, int amount);
	int GetRawItemAC();
	uint32 AccountID();
	const char *AccountName();
	int16 Admin();
	uint32 CharacterID();
	int GuildRank();
	uint32 GuildID();
	int GetFace();
	bool TakeMoneyFromPP(uint32 copper);
	bool TakeMoneyFromPP(uint32 copper, bool update_client);
	void AddPlatinum(uint32 platinum);
	void AddPlatinum(uint32 platinum, bool update_client);

	void AddMoneyToPP(uint32 copper, uint32 silver, uint32 gold, uint32 platinum, bool update_client);
	void AddMoneyToPP(uint32 copper, uint32 silver, uint32 gold, uint32 platinum);
	bool TakePlatinum(uint32 platinum);
	bool TakePlatinum(uint32 platinum, bool update_client);
	bool TGB();
	int GetSkillPoints();
	void SetSkillPoints(int skill);
	void IncreaseSkill(int skill_id);
	void IncreaseSkill(int skill_id, int value);
	void IncreaseLanguageSkill(int skill_id);
	void IncreaseLanguageSkill(int skill_id, int value);
	int GetRawSkill(int skill_id);
	bool HasSkill(int skill_id);
	bool CanHaveSkill(int skill_id);
	void SetSkill(int skill_id, int value);
	void AddSkill(int skill_id, int value);
	void CheckSpecializeIncrease(int spell_id);
	void CheckIncreaseSkill(int skill_id, Lua_Mob target);
	void CheckIncreaseSkill(int skill_id, Lua_Mob target, int chance_mod);
	void SetLanguageSkill(int language, int value);
	int MaxSkill(int skill_id);
	bool IsMedding();
	int GetDuelTarget();
	bool IsDueling();
	void SetDuelTarget(int c);
	void SetDueling(bool v);
	void ResetAA();
	void MemSpell(int spell_id, int slot);
	void MemSpell(int spell_id, int slot, bool update_client);
	void UnmemSpell(int slot);
	void UnmemSpell(int slot, bool update_client);
	void UnmemSpellBySpellID(int32 spell_id);
	void UnmemSpellAll();
	void UnmemSpellAll(bool update_client);
	int FindEmptyMemSlot();
	uint16 FindMemmedSpellBySlot(int slot);
	int FindMemmedSpellBySpellID(uint16 spell_id);
	int MemmedCount();
	luabind::object GetLearnableDisciplines(lua_State* L);
	luabind::object GetLearnableDisciplines(lua_State* L, uint8 min_level);
	luabind::object GetLearnableDisciplines(lua_State* L, uint8 min_level, uint8 max_level);
	luabind::object GetLearnedDisciplines(lua_State* L);
	luabind::object GetMemmedSpells(lua_State* L);
	luabind::object GetScribedSpells(lua_State* L);
	luabind::object GetScribeableSpells(lua_State* L);
	luabind::object GetScribeableSpells(lua_State* L, uint8 min_level);
	luabind::object GetScribeableSpells(lua_State* L, uint8 min_level, uint8 max_level);
	void ScribeSpell(int spell_id, int slot);
	void ScribeSpell(int spell_id, int slot, bool update_client);
	uint16 ScribeSpells(uint8 min_level, uint8 max_level);
	void UnscribeSpell(int slot);
	void UnscribeSpell(int slot, bool update_client);
	void UnscribeSpellAll();
	void UnscribeSpellAll(bool update_client);
	void UnscribeSpellBySpellID(uint16 spell_id);
	void UnscribeSpellBySpellID(uint16 spell_id, bool update_client);
	void TrainDisc(int itemid);
	uint16 LearnDisciplines(uint8 min_level, uint8 max_level);
	void TrainDiscBySpellID(int32 spell_id);
	int GetDiscSlotBySpellID(int32 spell_id);
	void UntrainDisc(int slot);
	void UntrainDisc(int slot, bool update_client);
	void UntrainDiscBySpellID(uint16 spell_id);
	void UntrainDiscBySpellID(uint16 spell_id, bool update_client);
	void UntrainDiscAll();
	void UntrainDiscAll(bool update_client);
	bool IsStanding();
	bool IsSitting();
	bool IsCrouching();
	void SetFeigned(bool v);
	bool GetFeigned();
	bool AutoSplitEnabled();
	void SetHorseId(int id);
	int GetHorseId();
	void NukeItem(uint32 item_num);
	void NukeItem(uint32 item_num, int where_to_check);
	void SetTint(int slot_id, uint32 color);
	void SetMaterial(int slot_id, uint32 item_id);
	void Undye();
	int GetItemIDAt(int slot_id);
	int GetAugmentIDAt(int slot_id, int aug_slot);
	void DeleteItemInInventory(int slot_id, int quantity);
	void DeleteItemInInventory(int slot_id, int quantity, bool update_client);
	void SummonItem(uint32 item_id);
	void SummonItem(uint32 item_id, int charges);
	void SummonItem(uint32 item_id, int charges, uint32 aug1);
	void SummonItem(uint32 item_id, int charges, uint32 aug1, uint32 aug2);
	void SummonItem(uint32 item_id, int charges, uint32 aug1, uint32 aug2, uint32 aug3);
	void SummonItem(uint32 item_id, int charges, uint32 aug1, uint32 aug2, uint32 aug3, uint32 aug4);
	void SummonItem(uint32 item_id, int charges, uint32 aug1, uint32 aug2, uint32 aug3, uint32 aug4, uint32 aug5);
	void SummonItem(uint32 item_id, int charges, uint32 aug1, uint32 aug2, uint32 aug3, uint32 aug4, uint32 aug5,
		bool attuned);
	void SummonItem(uint32 item_id, int charges, uint32 aug1, uint32 aug2, uint32 aug3, uint32 aug4, uint32 aug5,
		bool attuned, int to_slot);
	void SummonBaggedItems(uint32 bag_item_id, luabind::adl::object bag_items_table);
	void SetStats(int type, int value);
	void IncStats(int type, int value);
	void DropItem(int slot_id);
	void BreakInvis();
	void LeaveGroup();
	bool IsGrouped();
	bool IsRaidGrouped();
	bool Hungry();
	bool Thirsty();
	int GetInstrumentMod(int spell_id);
	bool DecreaseByID(uint32 type, int amt);
	void Escape();
	void GoFish();
	void ForageItem();
	void ForageItem(bool guarantee);
	float CalcPriceMod(Lua_Mob other, bool reverse);
	void ResetTrade();
	uint32 GetDisciplineTimer(uint32 timer_id);
	void ResetDisciplineTimer(uint32 timer_id);
	void ResetCastbarCooldownBySlot(int slot);
	void ResetAllCastbarCooldowns();
	void ResetCastbarCooldownBySpellID(uint32 spell_id);
	void ResetAllDisciplineTimers();
	bool UseDiscipline(int spell_id, int target_id);
	bool HasDisciplineLearned(uint16 spell_id);
	int GetCharacterFactionLevel(int faction_id);
	void ClearZoneFlag(uint32 zone_id);
	bool HasZoneFlag(uint32 zone_id);
	void LoadZoneFlags();
	void SendZoneFlagInfo(Lua_Client to);
	void SetZoneFlag(uint32 zone_id);
	void ClearPEQZoneFlag(uint32 zone_id);
	bool HasPEQZoneFlag(uint32 zone_id);
	void LoadPEQZoneFlags();
	void SendPEQZoneFlagInfo(Lua_Client to);
	void SetPEQZoneFlag(uint32 zone_id);
	void SetAATitle(std::string title);
	void SetAATitle(std::string title, bool save_to_database);
	int GetClientVersion();
	uint32 GetClientVersionBit();
	void SetTitleSuffix(const char *text);
	void SetAAPoints(int points);
	int GetAAPoints();
	int GetSpentAA();
	void AddAAPoints(int points);
	void RefundAA();
	int GetModCharacterFactionLevel(int faction);
	int GetLDoNWins();
	int GetLDoNLosses();
	int GetLDoNWinsTheme(int theme);
	int GetLDoNLossesTheme(int theme);
	int GetStartZone();
	void SetStartZone(int zone_id);
	void SetStartZone(int zone_id, float x);
	void SetStartZone(int zone_id, float x, float y);
	void SetStartZone(int zone_id, float x, float y, float z);
	void SetStartZone(int zone_id, float x, float y, float z, float heading);
	void KeyRingAdd(uint32 item);
	bool KeyRingCheck(uint32 item);
	void AddPVPPoints(uint32 points);
	void AddCrystals(uint32 radiant, uint32 ebon);
	void SetEbonCrystals(uint32 value);
	void SetRadiantCrystals(uint32 value);
	uint32 GetPVPPoints();
	uint32 GetRadiantCrystals();
	uint32 GetEbonCrystals();
	void QuestReadBook(const char *text, int type);
	void ReadBookByName(std::string book_name, uint8 book_type);
	void UpdateGroupAAs(int points, uint32 type);
	uint32 GetGroupPoints();
	uint32 GetRaidPoints();
	void LearnRecipe(uint32 recipe_id);
	int GetRecipeMadeCount(uint32 recipe_id);
	bool HasRecipeLearned(uint32 recipe_id);
	int GetEndurance();
	int GetMaxEndurance();
	int GetEndurancePercent();
	void SetEndurance(int endur);
	void SendOPTranslocateConfirm(Lua_Mob caster, int spell_id);
	uint32 GetIP();
	std::string GetIPString();
	int GetIPExemption();
	void SetIPExemption(int exemption_amount);
	void AddLevelBasedExp(int exp_pct);
	void AddLevelBasedExp(int exp_pct, int max_level);
	void AddLevelBasedExp(int exp_pct, int max_level, bool ignore_mods);
	void IncrementAA(int aa);
	bool GrantAlternateAdvancementAbility(int aa_id, int points);
	bool GrantAlternateAdvancementAbility(int aa_id, int points, bool ignore_cost);
	void ResetAlternateAdvancementRank(int aa_id);
	void MarkSingleCompassLoc(float in_x, float in_y, float in_z);
	void MarkSingleCompassLoc(float in_x, float in_y, float in_z, int count);
	void ClearCompassMark();
	int GetNextAvailableSpellBookSlot();
	int GetNextAvailableSpellBookSlot(int start);
	int GetNextAvailableDisciplineSlot();
	int GetNextAvailableDisciplineSlot(int starting_slot);
	uint32 GetSpellIDByBookSlot(int book_slot);
	int FindSpellBookSlotBySpellID(int spell_id);
	void UpdateTaskActivity(int task, int activity, int count);
	void AssignTask(int task_id);
	void AssignTask(int task_id, int npc_id);
	void AssignTask(int task_id, int npc_id, bool enforce_level_requirement);
	void FailTask(int task);
	bool IsTaskCompleted(int task);
	bool IsTaskActive(int task);
	bool IsTaskActivityActive(int task, int activity);
	void LockSharedTask(bool lock);
	void EndSharedTask();
	void EndSharedTask(bool send_fail);
	int GetCorpseCount();
	int GetCorpseID(int corpse);
	int GetCorpseItemAt(int corpse, int slot);
	void AssignToInstance(int instance_id);
	void Freeze();
	void UnFreeze();
	int GetAggroCount();
	uint64 GetCarriedMoney();
	uint32 GetCarriedPlatinum();
	uint64 GetAllMoney();
	uint32 GetMoney(uint8 type, uint8 subtype);
	void OpenLFGuildWindow();
	void NotifyNewTitlesAvailable();
	void Signal(int signal_id);
	void AddAlternateCurrencyValue(uint32 currency, int amount);
	void SetAlternateCurrencyValue(uint32 currency, int amount);
	int GetAlternateCurrencyValue(uint32 currency);
	void SendWebLink(const char *site);
	bool HasSpellScribed(int spell_id);
	void SetAccountFlag(std::string flag, std::string val);
	std::string GetAccountFlag(std::string flag);
	int GetAccountAge();
	Lua_Group GetGroup();
	Lua_Raid GetRaid();
	bool PutItemInInventory(int slot_id, Lua_ItemInst inst);
	bool PushItemOnCursor(Lua_ItemInst inst);
	Lua_Inventory GetInventory();
	void SendItemScale(Lua_ItemInst inst);
	void QueuePacket(Lua_Packet app);
	void QueuePacket(Lua_Packet app, bool ack_req);
	void QueuePacket(Lua_Packet app, bool ack_req, int client_connection_status);
	void QueuePacket(Lua_Packet app, bool ack_req, int client_connection_status, int filter);
	int GetHunger();
	int GetThirst();
	void SetHunger(int in_hunger);
	void SetThirst(int in_thirst);
	void SetConsumption(int in_hunger, int in_thirst);
	void SendMarqueeMessage(uint32 type, std::string message);
	void SendMarqueeMessage(uint32 type, std::string message, uint32 duration);
	void SendMarqueeMessage(uint32 type, uint32 priority, uint32 fade_in, uint32 fade_out, uint32 duration, std::string message);
	void SendColoredText(uint32 type, std::string msg);
	void PlayMP3(std::string file);
	void QuestReward(Lua_Mob target);
	void QuestReward(Lua_Mob target, uint32 copper);
	void QuestReward(Lua_Mob target, uint32 copper, uint32 silver);
	void QuestReward(Lua_Mob target, uint32 copper, uint32 silver, uint32 gold);
	void QuestReward(Lua_Mob target, uint32 copper, uint32 silver, uint32 gold, uint32 platinum);
	void QuestReward(Lua_Mob target, uint32 copper, uint32 silver, uint32 gold, uint32 platinum, uint32 itemid);
	void QuestReward(Lua_Mob target, uint32 copper, uint32 silver, uint32 gold, uint32 platinum, uint32 itemid, uint32 exp);
	void QuestReward(Lua_Mob target, uint32 copper, uint32 silver, uint32 gold, uint32 platinum, uint32 itemid, uint32 exp, bool faction);
	void QuestReward(Lua_Mob target, luabind::adl::object reward);
	void CashReward(uint32 copper, uint32 silver, uint32 gold, uint32 platinum);
	bool IsDead();
	int CalcCurrentWeight();
	int CalcATK();
	void FilteredMessage(Mob *sender, uint32 type, int filter, const char* message);
	void EnableAreaHPRegen(int value);
	void DisableAreaHPRegen();
	void EnableAreaManaRegen(int value);
	void DisableAreaManaRegen();
	void EnableAreaEndRegen(int value);
	void DisableAreaEndRegen();
	void EnableAreaRegens(int value);
	void DisableAreaRegens();
	void SetHideMe(bool hide_me_state);
	void Popup(const char* title, const char* text);
	void Popup(const char* title, const char* text, uint32 popup_id);
	void Popup(const char* title, const char* text, uint32 popup_id, uint32 negative_id);
	void Popup(const char* title, const char* text, uint32 popup_id, uint32 negative_id, uint32 button_type);
	void Popup(const char* title, const char* text, uint32 popup_id, uint32 negative_id, uint32 button_type, uint32 duration);
	void Popup(const char* title, const char* text, uint32 popup_id, uint32 negative_id, uint32 button_type, uint32 duration, const char* button_name_one, const char* button_name_two);
	void Popup(const char* title, const char* text, uint32 popup_id, uint32 negative_id, uint32 button_type, uint32 duration, const char* button_name_one, const char* button_name_two, uint32 sound_controls);
	int CountItem(uint32 item_id);
	void RemoveItem(uint32 item_id);
	void RemoveItem(uint32 item_id, uint32 quantity);
	void SetGMStatus(int16 new_status);
	int16 GetGMStatus();
	void AddItem(luabind::object item_table);
	int CountAugmentEquippedByID(uint32 item_id);
	int CountItemEquippedByID(uint32 item_id);
	bool HasAugmentEquippedByID(uint32 item_id);
	bool HasItemEquippedByID(uint32 item_id);
	int GetHealAmount();
	int GetSpellDamage();
	void UpdateAdmin();
	void UpdateAdmin(bool from_database);
	luabind::object GetPEQZoneFlags(lua_State* L);
	luabind::object GetZoneFlags(lua_State* L);
	void SendPayload(int payload_id);
	void SendPayload(int payload_id, std::string payload_value);
	std::string GetGuildPublicNote();

	void ApplySpell(int spell_id);
	void ApplySpell(int spell_id, int duration);
	void ApplySpell(int spell_id, int duration, bool allow_pets);
#ifdef BOTS
	void ApplySpell(int spell_id, int duration, bool allow_pets, bool allow_bots);
#endif

	void ApplySpellGroup(int spell_id);
	void ApplySpellGroup(int spell_id, int duration);
	void ApplySpellGroup(int spell_id, int duration, bool allow_pets);
#ifdef BOTS
	void ApplySpellGroup(int spell_id, int duration, bool allow_pets, bool allow_bots);
#endif

	void ApplySpellRaid(int spell_id);
	void ApplySpellRaid(int spell_id, int duration);
	void ApplySpellRaid(int spell_id, int duration, bool allow_pets);
	void ApplySpellRaid(int spell_id, int duration, bool allow_pets, bool is_raid_group_only);
#ifdef BOTS
	void ApplySpellRaid(int spell_id, int duration, bool allow_pets, bool is_raid_group_only, bool allow_bots);
#endif

	void SetSpellDuration(int spell_id);
	void SetSpellDuration(int spell_id, int duration);
	void SetSpellDuration(int spell_id, int duration, bool allow_pets);
#ifdef BOTS
	void SetSpellDuration(int spell_id, int duration, bool allow_pets, bool allow_bots);
#endif

	void SetSpellDurationGroup(int spell_id);
	void SetSpellDurationGroup(int spell_id, int duration);
	void SetSpellDurationGroup(int spell_id, int duration, bool allow_pets);
#ifdef BOTS
	void SetSpellDurationGroup(int spell_id, int duration, bool allow_pets, bool allow_bots);
#endif

	void SetSpellDurationRaid(int spell_id);
	void SetSpellDurationRaid(int spell_id, int duration);
	void SetSpellDurationRaid(int spell_id, int duration, bool allow_pets);
	void SetSpellDurationRaid(int spell_id, int duration, bool allow_pets, bool is_raid_group_only);
#ifdef BOTS
	void SetSpellDurationRaid(int spell_id, int duration, bool allow_pets, bool is_raid_group_only, bool allow_bots);
#endif


	int GetEnvironmentDamageModifier();
	void SetEnvironmentDamageModifier(int value);
	bool GetInvulnerableEnvironmentDamage();
	void SetInvulnerableEnvironmentDamage(bool value);

	void SetPrimaryWeaponOrnamentation(uint32 model_id);
	void SetSecondaryWeaponOrnamentation(uint32 model_id);
	void TaskSelector(luabind::adl::object table);
	void TaskSelector(luabind::adl::object table, bool ignore_cooldown);

	void SetClientMaxLevel(uint8 max_level);
	uint8 GetClientMaxLevel();

	bool SendGMCommand(std::string message);
	bool SendGMCommand(std::string message, bool ignore_status);

#ifdef BOTS

	int GetBotRequiredLevel();
	int GetBotRequiredLevel(uint8 class_id);
	uint32 GetBotCreationLimit();
	uint32 GetBotCreationLimit(uint8 class_id);
	int GetBotSpawnLimit();
	int GetBotSpawnLimit(uint8 class_id);
	void SetBotRequiredLevel(int new_required_level);
	void SetBotRequiredLevel(int new_required_level, uint8 class_id);
	void SetBotCreationLimit(uint32 new_creation_limit);
	void SetBotCreationLimit(uint32 new_creation_limit, uint8 class_id);
	void SetBotSpawnLimit(int new_spawn_limit);
	void SetBotSpawnLimit(int new_spawn_limit, uint8 class_id);

#endif

	void DialogueWindow(std::string markdown);

	Lua_Expedition  CreateExpedition(luabind::object expedition_info);
	Lua_Expedition  CreateExpedition(std::string zone_name, uint32 version, uint32 duration, std::string expedition_name, uint32 min_players, uint32 max_players);
	Lua_Expedition  CreateExpedition(std::string zone_name, uint32 version, uint32 duration, std::string expedition_name, uint32 min_players, uint32 max_players, bool disable_messages);
	Lua_Expedition  CreateExpeditionFromTemplate(uint32_t dz_template_id);
	Lua_Expedition  GetExpedition();
	luabind::object GetExpeditionLockouts(lua_State* L);
	luabind::object GetExpeditionLockouts(lua_State* L, std::string expedition_name);
	std::string     GetLockoutExpeditionUUID(std::string expedition_name, std::string event_name);
	void            AddExpeditionLockout(std::string expedition_name, std::string event_name, uint32 seconds);
	void            AddExpeditionLockout(std::string expedition_name, std::string event_name, uint32 seconds, std::string uuid);
	void            AddExpeditionLockoutDuration(std::string expedition_name, std::string event_name, int seconds);
	void            AddExpeditionLockoutDuration(std::string expedition_name, std::string event_name, int seconds, std::string uuid);
	void            RemoveAllExpeditionLockouts();
	void            RemoveAllExpeditionLockouts(std::string expedition_name);
	void            RemoveExpeditionLockout(std::string expedition_name, std::string event_name);
	bool            HasExpeditionLockout(std::string expedition_name, std::string event_name);
	void            MovePCDynamicZone(uint32 zone_id);
	void            MovePCDynamicZone(uint32 zone_id, int zone_version);
	void            MovePCDynamicZone(uint32 zone_id, int zone_version, bool msg_if_invalid);
	void            MovePCDynamicZone(std::string zone_name);
	void            MovePCDynamicZone(std::string zone_name, int zone_version);
	void            MovePCDynamicZone(std::string zone_name, int zone_version, bool msg_if_invalid);
	void            CreateTaskDynamicZone(int task_id, luabind::object dz_table);
	void            Fling(float value, float target_x, float target_y, float target_z);
	void            Fling(float value, float target_x, float target_y, float target_z, bool ignore_los);
	void            Fling(float value, float target_x, float target_y, float target_z, bool ignore_los, bool clipping);
};

#endif
#endif
