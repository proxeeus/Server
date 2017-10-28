#ifndef EQEMU_LUA_MOB_H
#define EQEMU_LUA_MOB_H
#ifdef LUA_EQEMU

#include "lua_entity.h"

class Mob;
struct Lua_HateList;
class Lua_Item;
class Lua_ItemInst;
class Lua_StatBonuses;

namespace luabind {
	struct scope;
	namespace adl {
		class object;
	}
}

luabind::scope lua_register_mob();
luabind::scope lua_register_special_abilities();

class Lua_Mob : public Lua_Entity
{
	typedef Mob NativeType;
public:
	Lua_Mob() { SetLuaPtrData(nullptr); }
	Lua_Mob(Mob *d) { SetLuaPtrData(reinterpret_cast<Entity*>(d)); }
	virtual ~Lua_Mob() { }

	operator Mob*() {
		return reinterpret_cast<Mob*>(GetLuaPtrData());
	}

	const char *GetName();
	void Depop();
	void Depop(bool start_spawn_timer);
	bool BehindMob();
	bool BehindMob(Lua_Mob other);
	bool BehindMob(Lua_Mob other, float x);
	bool BehindMob(Lua_Mob other, float x, float y);
	void SetLevel(int level);
	void SetLevel(int level, bool command);
	void SendWearChange(int material_slot);
	bool IsMoving();
	bool IsFeared();
	bool IsBlind();
	void GotoBind();
	void Gate();
	bool Attack(Lua_Mob other);
	bool Attack(Lua_Mob other, int hand);
	bool Attack(Lua_Mob other, int hand, bool from_riposte);
	bool Attack(Lua_Mob other, int hand, bool from_riposte, bool is_strikethrough);
	bool Attack(Lua_Mob other, int hand, bool from_riposte, bool is_strikethrough, bool is_from_spell);
	bool Attack(Lua_Mob other, int hand, bool from_riposte, bool is_strikethrough, bool is_from_spell, luabind::adl::object opts);
	void Damage(Lua_Mob from, int damage, int spell_id, int attack_skill);
	void Damage(Lua_Mob from, int damage, int spell_id, int attack_skill, bool avoidable);
	void Damage(Lua_Mob from, int damage, int spell_id, int attack_skill, bool avoidable, int buffslot);
	void Damage(Lua_Mob from, int damage, int spell_id, int attack_skill, bool avoidable, int buffslot, bool buff_tic);
	void RangedAttack(Lua_Mob other);
	void ThrowingAttack(Lua_Mob other);
	void Heal();
	void HealDamage(uint32 amount);
	void HealDamage(uint32 amount, Lua_Mob other);
	uint32 GetLevelCon(int other);
	uint32 GetLevelCon(int my, int other);
	void SetHP(int hp);
	void DoAnim(int anim_num);
	void DoAnim(int anim_num, int type);
	void DoAnim(int anim_num, int type, bool ackreq);
	void DoAnim(int anim_num, int type, bool ackreq, int filter);
	void ChangeSize(double in_size);
	void ChangeSize(double in_size, bool no_restriction);
	void GMMove(double x, double y, double z);
	void GMMove(double x, double y, double z, double heading);
	void GMMove(double x, double y, double z, double heading, bool send_update);
	bool HasProcs();
	bool IsInvisible();
	bool IsInvisible(Lua_Mob other);
    void SetInvisible(int state);
	bool FindBuff(int spell_id);
    bool FindType(int type);
	bool FindType(int type, bool offensive);
	bool FindType(int type, bool offensive, int threshold);
	int GetBuffSlotFromType(int slot);
	int GetBaseRace();
	int GetBaseGender();
	int GetDeity();
	int GetRace();
	int GetGender();
	int GetTexture();
	int GetHelmTexture();
	int GetHairColor();
	int GetBeardColor();
	int GetEyeColor1();
	int GetEyeColor2();
	int GetHairStyle();
	int GetLuclinFace();
	int GetBeard();
	int GetDrakkinHeritage();
	int GetDrakkinTattoo();
	int GetDrakkinDetails();
	int GetClass();
	int GetLevel();
	const char *GetCleanName();
	Lua_Mob GetTarget();
	void SetTarget(Lua_Mob t);
	double GetHPRatio();
	bool IsWarriorClass();
	int GetHP();
	int GetMaxHP();
	int GetItemHPBonuses();
	int GetSpellHPBonuses();
	double GetWalkspeed();
	double GetRunspeed();
	int GetCasterLevel(int spell_id);
	int GetMaxMana();
	int GetMana();
	int SetMana(int mana);
	double GetManaRatio();
	int GetAC();
	int GetATK();
	int GetSTR();
	int GetSTA();
	int GetDEX();
	int GetAGI();
	int GetINT();
	int GetWIS();
	int GetCHA();
	int GetMR();
	int GetFR();
	int GetDR();
	int GetPR();
	int GetCR();
	int GetCorruption();
	int GetPhR();
	int GetMaxSTR();
	int GetMaxSTA();
	int GetMaxDEX();
	int GetMaxAGI();
	int GetMaxINT();
	int GetMaxWIS();
	int GetMaxCHA();
	double ResistSpell(int resist_type, int spell_id, Lua_Mob caster);
	double ResistSpell(int resist_type, int spell_id, Lua_Mob caster, bool use_resist_override);
	double ResistSpell(int resist_type, int spell_id, Lua_Mob caster, bool use_resist_override, int resist_override);
	double ResistSpell(int resist_type, int spell_id, Lua_Mob caster, bool use_resist_override, int resist_override, bool charisma_check);
	int GetSpecializeSkillValue(int spell_id);
	int GetNPCTypeID();
	bool IsTargeted();
	double GetX();
	double GetY();
	double GetZ();
	double GetHeading();
	double GetWaypointX();
	double GetWaypointY();
	double GetWaypointZ();
	double GetWaypointH();
	double GetWaypointPause();
	int GetWaypointID();
	void SetCurrentWP(int wp);
	double GetSize();
	void Message(int type, const char *message);
	void Message_StringID(int type, int string_id, uint32 distance);
	void Say(const char *message);
	void QuestSay(Lua_Client client, const char *message);
	void Shout(const char *message);
	void Emote(const char *message);
	void InterruptSpell();
	void InterruptSpell(int spell_id);
	bool CastSpell(int spell_id, int target_id);
	bool CastSpell(int spell_id, int target_id, int slot);
	bool CastSpell(int spell_id, int target_id, int slot, int cast_time);
	bool CastSpell(int spell_id, int target_id, int slot, int cast_time, int mana_cost);
	bool CastSpell(int spell_id, int target_id, int slot, int cast_time, int mana_cost, int item_slot);
	bool CastSpell(int spell_id, int target_id, int slot, int cast_time, int mana_cost, int item_slot, int timer, int timer_duration);
	bool CastSpell(int spell_id, int target_id, int slot, int cast_time, int mana_cost, int item_slot, int timer, int timer_duration,
		int resist_adjust);
	bool SpellFinished(int spell_id, Lua_Mob target);
	bool SpellFinished(int spell_id, Lua_Mob target, int slot);
	bool SpellFinished(int spell_id, Lua_Mob target, int slot, int mana_used);
	bool SpellFinished(int spell_id, Lua_Mob target, int slot, int mana_used, uint32 inventory_slot);
	bool SpellFinished(int spell_id, Lua_Mob target, int slot, int mana_used, uint32 inventory_slot, int resist_adjust);
	bool SpellFinished(int spell_id, Lua_Mob target, int slot, int mana_used, uint32 inventory_slot, int resist_adjust, bool proc);
	void SendBeginCast(int spell_id, int cast_time);
	void SpellEffect(Lua_Mob caster, int spell_id, double partial);
	Lua_Mob GetPet();
	Lua_Mob GetOwner();
	Lua_HateList GetHateList();
	Lua_Mob GetHateTop();
	Lua_Mob GetHateDamageTop(Lua_Mob other);
	Lua_Mob GetHateRandom();
	void AddToHateList(Lua_Mob other);
	void AddToHateList(Lua_Mob other, int hate);
	void AddToHateList(Lua_Mob other, int hate, int damage);
	void AddToHateList(Lua_Mob other, int hate, int damage, bool yell_for_help);
	void AddToHateList(Lua_Mob other, int hate, int damage, bool yell_for_help, bool frenzy);
	void AddToHateList(Lua_Mob other, int hate, int damage, bool yell_for_help, bool frenzy, bool buff_tic);
	void SetHate(Lua_Mob other);
	void SetHate(Lua_Mob other, int hate);
	void SetHate(Lua_Mob other, int hate, int damage);
	void HalveAggro(Lua_Mob other);
	void DoubleAggro(Lua_Mob other);
	uint32 GetHateAmount(Lua_Mob target);
	uint32 GetHateAmount(Lua_Mob target, bool is_damage);
	uint32 GetDamageAmount(Lua_Mob target);
	void WipeHateList();
	bool CheckAggro(Lua_Mob other);
	void Stun(int duration);
	void UnStun();
	bool IsStunned();
	void Spin();
	void Kill();
	bool CanThisClassDoubleAttack();
	bool CanThisClassDualWield();
	bool CanThisClassRiposte();
	bool CanThisClassDodge();
	bool CanThisClassParry();
	bool CanThisClassBlock();
	void SetInvul(bool value);
	bool GetInvul();
	void SetExtraHaste(int haste);
	int GetHaste();
	int GetHandToHandDamage();
	int GetHandToHandDelay();
	void Mesmerize();
	bool IsMezzed();
	bool IsEnraged();
	int GetReverseFactionCon(Lua_Mob other);
	bool IsAIControlled();
	float GetAggroRange();
	float GetAssistRange();
	void SetPetOrder(int order);
	int GetPetOrder();
	bool IsRoamer();
	bool IsRooted();
	bool IsEngaged();
	void FaceTarget(Lua_Mob target);
	void SetHeading(double in);
	double CalculateHeadingToTarget(double in_x, double in_y);
	bool CalculateNewPosition(double x, double y, double z, double speed);
	bool CalculateNewPosition(double x, double y, double z, double speed, bool check_z);
	bool CalculateNewPosition2(double x, double y, double z, double speed);
	bool CalculateNewPosition2(double x, double y, double z, double speed, bool check_z);
	float CalculateDistance(double x, double y, double z);
	void SendTo(double x, double y, double z);
	void SendToFixZ(double x, double y, double z);
	void NPCSpecialAttacks(const char *parse, int perm);
	void NPCSpecialAttacks(const char *parse, int perm, bool reset);
	void NPCSpecialAttacks(const char *parse, int perm, bool reset, bool remove);
	int GetResist(int type);
	bool Charmed();
	int CheckAggroAmount(int spell_id);
	int CheckAggroAmount(int spell_id, bool is_proc);
	int CheckHealAggroAmount(int spell_id);
	int CheckHealAggroAmount(int spell_id, uint32 heal_possible);
	int GetAA(int id);
	int GetAAByAAID(int id);
	bool SetAA(int rank_id, int new_value);
	bool SetAA(int rank_id, int new_value, int charges);
	bool DivineAura();
	void SetOOCRegen(int regen);
	const char* GetEntityVariable(const char *name);
	void SetEntityVariable(const char *name, const char *value);
	bool EntityVariableExists(const char *name);
	void Signal(uint32 id);
	bool CombatRange(Lua_Mob other);
	void DoSpecialAttackDamage(Lua_Mob other, int skill, int max_damage);
	void DoSpecialAttackDamage(Lua_Mob other, int skill, int max_damage, int min_damage);
	void DoSpecialAttackDamage(Lua_Mob other, int skill, int max_damage, int min_damage, int hate_override);
	void DoSpecialAttackDamage(Lua_Mob other, int skill, int max_damage, int min_damage, int hate_override, int reuse_time);
	void DoThrowingAttackDmg(Lua_Mob other);
	void DoThrowingAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon);
	void DoThrowingAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon, Lua_Item item);
	void DoThrowingAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon, Lua_Item item, int weapon_damage);
	void DoThrowingAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon, Lua_Item item, int weapon_damage, int chance_mod);
	void DoThrowingAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon, Lua_Item item, int weapon_damage, int chance_mod, int focus);
	void DoMeleeSkillAttackDmg(Lua_Mob other, int weapon_damage, int skill);
	void DoMeleeSkillAttackDmg(Lua_Mob other, int weapon_damage, int skill, int chance_mod);
	void DoMeleeSkillAttackDmg(Lua_Mob other, int weapon_damage, int skill, int chance_mod, int focus);
	void DoMeleeSkillAttackDmg(Lua_Mob other, int weapon_damage, int skill, int chance_mod, int focus, bool can_riposte);
	void DoArcheryAttackDmg(Lua_Mob other);
	void DoArcheryAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon);
	void DoArcheryAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon, Lua_ItemInst ammo);
	void DoArcheryAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon, Lua_ItemInst ammo, int weapon_damage);
	void DoArcheryAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon, Lua_ItemInst ammo, int weapon_damage, int chance_mod);
	void DoArcheryAttackDmg(Lua_Mob other, Lua_ItemInst range_weapon, Lua_ItemInst ammo, int weapon_damage, int chance_mod, int focus);
	bool CheckLoS(Lua_Mob other);
	bool CheckLoSToLoc(double x, double y, double z);
	bool CheckLoSToLoc(double x, double y, double z, double mob_size);
	double FindGroundZ(double x, double y);
	double FindGroundZ(double x, double y, double z);
	void ProjectileAnimation(Lua_Mob to, int item_id);
	void ProjectileAnimation(Lua_Mob to, int item_id, bool is_arrow);
	void ProjectileAnimation(Lua_Mob to, int item_id, bool is_arrow, double speed);
	void ProjectileAnimation(Lua_Mob to, int item_id, bool is_arrow, double speed, double angle);
	void ProjectileAnimation(Lua_Mob to, int item_id, bool is_arrow, double speed, double angle, double tilt);
	void ProjectileAnimation(Lua_Mob to, int item_id, bool is_arrow, double speed, double angle, double tilt, double arc);
	bool HasNPCSpecialAtk(const char *parse);
	void SendAppearanceEffect(uint32 parm1, uint32 parm2, uint32 parm3, uint32 parm4, uint32 parm5);
	void SendAppearanceEffect(uint32 parm1, uint32 parm2, uint32 parm3, uint32 parm4, uint32 parm5, Lua_Client specific_target);
	void SetFlyMode(int in);
	void SetTexture(int in);
	void SetRace(int in);
	void SetGender(int in);
	void SendIllusionPacket(luabind::adl::object illusion);
	void CameraEffect(uint32 duration, uint32 intensity);
	void CameraEffect(uint32 duration, uint32 intensity, Lua_Client c);
	void CameraEffect(uint32 duration, uint32 intensity, Lua_Client c, bool global);
	void SendSpellEffect(uint32 effect_id, uint32 duration, uint32 finish_delay, bool zone_wide,
		uint32 unk020);
	void SendSpellEffect(uint32 effect_id, uint32 duration, uint32 finish_delay, bool zone_wide,
		uint32 unk020, bool perm_effect);
	void SendSpellEffect(uint32 effect_id, uint32 duration, uint32 finish_delay, bool zone_wide,
		uint32 unk020, bool perm_effect, Lua_Client c);
	void TempName();
	void TempName(const char *newname);
	std::string GetGlobal(const char *varname);
	void SetGlobal(const char *varname, const char *newvalue, int options, const char *duration);
	void SetGlobal(const char *varname, const char *newvalue, int options, const char *duration, Lua_Mob other);
	void TarGlobal(const char *varname, const char *value, const char *duration, int npc_id, int char_id, int zone_id);
	void DelGlobal(const char *varname);
	void SetSlotTint(int material_slot, int red_tint, int green_tint, int blue_tint);
	void WearChange(int material_slot, int texture, uint32 color);
	void DoKnockback(Lua_Mob caster, uint32 pushback, uint32 pushup);
	void AddNimbusEffect(int effect_id);
	void RemoveNimbusEffect(int effect_id);
	bool IsRunning();
	void SetRunning(bool running);
	void SetBodyType(int new_body, bool overwrite_orig);
	void SetTargetable(bool on);
	void ModSkillDmgTaken(int skill, int value);
	int GetModSkillDmgTaken(int skill);
	int GetSkillDmgTaken(int skill);
	int GetFcDamageAmtIncoming(Lua_Mob caster, uint32 spell_id, bool use_skill, uint16 skill);
	int GetSkillDmgAmt(uint16 skill);
	void SetAllowBeneficial(bool value);
	bool GetAllowBeneficial();
	bool IsBeneficialAllowed(Lua_Mob target);
	void ModVulnerability(int resist, int value);
	int GetModVulnerability(int resist);
	void SetDisableMelee(bool disable);
	bool IsMeleeDisabled();
	void SetFlurryChance(int value);
	int GetFlurryChance();
	int GetSkill(int skill_id);
	int GetSpecialAbility(int ability);
	int GetSpecialAbilityParam(int ability, int param);
	void SetSpecialAbility(int ability, int level);
	void SetSpecialAbilityParam(int ability, int param, int value);
	void ClearSpecialAbilities();
	void ProcessSpecialAbilities(std::string str);
	void SetAppearance(int app);
	uint32 GetAppearance();
	void SetAppearance(int app, bool ignore_self);
	void SetDestructibleObject(bool set);
	bool IsImmuneToSpell(int spell_id, Lua_Mob caster);
	void BuffFadeBySpellID(int spell_id);
	void BuffFadeByEffect(int effect_id);
	void BuffFadeByEffect(int effect_id, int skipslot);
	void BuffFadeAll();
	void BuffFadeBySlot(int slot);
	void BuffFadeBySlot(int slot, bool recalc_bonuses);
	int CanBuffStack(int spell_id, int caster_level);
	int CanBuffStack(int spell_id, int caster_level, bool fail_if_overwrite);
	void SetPseudoRoot(bool in);
	uint8 SeeInvisible();
	bool SeeInvisibleUndead();
	bool SeeHide();
	bool SeeImprovedHide();
	uint8 GetNimbusEffect1();
	uint8 GetNimbusEffect2();
	uint8 GetNimbusEffect3();
	bool IsTargetable();
	bool HasShieldEquiped();
	bool HasTwoHandBluntEquiped();
	bool HasTwoHanderEquipped();
	uint32 GetHerosForgeModel(uint8 material_slot);
	uint32 IsEliteMaterialItem(uint8 material_slot);
	float GetBaseSize();
	bool HasOwner();
	bool IsPet();
	bool HasPet();
	bool IsSilenced();
	bool IsAmnesiad();
	int32 GetMeleeMitigation();
	int GetWeaponDamageBonus(Lua_Item weapon, bool offhand);
	Lua_StatBonuses GetItemBonuses();
	Lua_StatBonuses GetSpellBonuses();
	Lua_StatBonuses GetAABonuses();
	int16 GetMeleeDamageMod_SE(uint16 skill);
	int16 GetMeleeMinDamageMod_SE(uint16 skill);
	bool IsAttackAllowed(Lua_Mob target, bool isSpellAttack);
	bool IsCasting();
	int AttackAnimation(int Hand, Lua_ItemInst weapon);
	int GetWeaponDamage(Lua_Mob against, Lua_ItemInst weapon);
	bool IsBerserk();
	bool TryFinishingBlow(Lua_Mob defender, int &damage);
	int GetBodyType();
	int GetOrigBodyType();
	void CheckNumHitsRemaining(int type, int32 buff_slot, uint16 spell_id);
};

#endif
#endif
