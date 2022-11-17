#ifndef COMMAND_H
#define COMMAND_H

class Client;
class Seperator;

#include "../common/types.h"
#include <string>

#define    COMMAND_CHAR '#'

typedef void (*CmdFuncPtr)(Client *, const Seperator *);

typedef struct {
	uint8 admin;
	std::string description;
	CmdFuncPtr function; // null means perl function
} CommandRecord;

extern int (*command_dispatch)(Client *,std::string);
extern int command_count; // Commands Loaded Count

// Command Utilities
int command_init(void);
void command_deinit(void);
int command_add(std::string command_name, std::string description, uint8 admin, CmdFuncPtr function);
int command_notavail(Client *c, std::string message);
int command_realdispatch(Client *c, std::string message);
void command_logcommand(Client *c, std::string message);
uint8 GetCommandStatus(Client *c, std::string command_name);
void ListModifyNPCStatMap(Client *c);
std::map<std::string, std::string> GetModifyNPCStatMap();
std::string GetModifyNPCStatDescription(std::string stat);

// Commands
void command_acceptrules(Client *c, const Seperator *sep);
void command_advnpcspawn(Client *c, const Seperator *sep);
void command_aggro(Client *c, const Seperator *sep);
void command_aggrozone(Client *c, const Seperator *sep);
void command_ai(Client *c, const Seperator *sep);
void command_appearance(Client *c, const Seperator *sep);
void command_appearanceeffects(Client *c, const Seperator *sep);
void command_apply_shared_memory(Client *c, const Seperator *sep);
void command_attack(Client *c, const Seperator *sep);
void command_augmentitem(Client *c, const Seperator *sep);
void command_ban(Client *c, const Seperator *sep);
void command_bind(Client *c, const Seperator *sep);
void command_camerashake(Client *c, const Seperator *sep);
void command_castspell(Client *c, const Seperator *sep);
void command_chat(Client *c, const Seperator *sep);
void command_checklos(Client *c, const Seperator *sep);
void command_copycharacter(Client *c, const Seperator *sep);
void command_corpse(Client *c, const Seperator *sep);
void command_corpsefix(Client *c, const Seperator *sep);
void command_countitem(Client *c, const Seperator *sep);
void command_cvs(Client *c, const Seperator *sep);
void command_damage(Client *c, const Seperator *sep);
void command_databuckets(Client *c, const Seperator *sep);
void command_date(Client *c, const Seperator *sep);
void command_dbspawn2(Client *c, const Seperator *sep);
void command_delacct(Client *c, const Seperator *sep);
void command_delpetition(Client *c, const Seperator *sep);
void command_depop(Client *c, const Seperator *sep);
void command_depopzone(Client *c, const Seperator *sep);
void command_devtools(Client *c, const Seperator *sep);
void command_disablerecipe(Client *c, const Seperator *sep);
void command_disarmtrap(Client *c, const Seperator *sep);
void command_door(Client *c, const Seperator *sep);
void command_distance(Client *c, const Seperator *sep);
void command_doanim(Client *c, const Seperator *sep);
void command_dye(Client *c, const Seperator *sep);
void command_dz(Client *c, const Seperator *sep);
void command_dzkickplayers(Client *c, const Seperator *sep);
void command_editmassrespawn(Client *c, const Seperator *sep);
void command_emote(Client *c, const Seperator *sep);
void command_emotesearch(Client *c, const Seperator *sep);
void command_emoteview(Client *c, const Seperator *sep);
void command_emptyinventory(Client *c, const Seperator *sep);
void command_enablerecipe(Client *c, const Seperator *sep);
void command_endurance(Client *c, const Seperator *sep);
void command_equipitem(Client *c, const Seperator *sep);
void command_faction(Client *c, const Seperator *sep);
void command_faction_association(Client *c, const Seperator *sep);
void command_feature(Client *c, const Seperator *sep);
void command_findaa(Client *c, const Seperator *sep);
void command_findaliases(Client *c, const Seperator *sep);
void command_findclass(Client *c, const Seperator *sep);
void command_findfaction(Client *c, const Seperator *sep);
void command_findnpctype(Client *c, const Seperator *sep);
void command_findrace(Client *c, const Seperator *sep);
void command_findrecipe(Client *c, const Seperator *sep);
void command_findskill(Client *c, const Seperator *sep);
void command_findspell(Client *c, const Seperator *sep);
void command_findtask(Client *c, const Seperator *sep);
void command_findzone(Client *c, const Seperator *sep);
void command_fixmob(Client *c, const Seperator *sep);
void command_flag(Client *c, const Seperator *sep);
void command_flagedit(Client *c, const Seperator *sep);
void command_flags(Client *c, const Seperator *sep);
void command_flymode(Client *c, const Seperator *sep);
void command_fov(Client *c, const Seperator *sep);
void command_freeze(Client *c, const Seperator *sep);
void command_gassign(Client *c, const Seperator *sep);
void command_gearup(Client *c, const Seperator *sep);
void command_gender(Client *c, const Seperator *sep);
void command_getplayerburiedcorpsecount(Client *c, const Seperator *sep);
void command_getvariable(Client *c, const Seperator *sep);
void command_ginfo(Client *c, const Seperator *sep);
void command_giveitem(Client *c, const Seperator *sep);
void command_givemoney(Client *c, const Seperator *sep);
void command_globalview(Client *c, const Seperator *sep);
void command_gm(Client *c, const Seperator *sep);
void command_gmspeed(Client *c, const Seperator *sep);
void command_gmzone(Client *c, const Seperator *sep);
void command_godmode(Client* c, const Seperator *sep);
void command_goto(Client *c, const Seperator *sep);
void command_grid(Client *c, const Seperator *sep);
void command_guild(Client *c, const Seperator *sep);
void command_guildapprove(Client *c, const Seperator *sep);
void command_guildcreate(Client *c, const Seperator *sep);
void command_guildlist(Client *c, const Seperator *sep);
void command_haste(Client *c, const Seperator *sep);
void command_hatelist(Client *c, const Seperator *sep);
void command_heal(Client *c, const Seperator *sep);
void command_help(Client *c, const Seperator *sep);
void command_heromodel(Client *c, const Seperator *sep);
void command_hideme(Client *c, const Seperator *sep);
void command_hotfix(Client *c, const Seperator *sep);
void command_hp(Client *c, const Seperator *sep);
void command_incstat(Client *c, const Seperator *sep);
void command_instance(Client *c, const Seperator *sep);
void command_interrogateinv(Client *c, const Seperator *sep);
void command_interrupt(Client *c, const Seperator *sep);
void command_invsnapshot(Client *c, const Seperator *sep);
void command_invul(Client *c, const Seperator *sep);
void command_ipban(Client *c, const Seperator *sep);
void command_iplookup(Client *c, const Seperator *sep);
void command_iteminfo(Client *c, const Seperator *sep);
void command_itemsearch(Client *c, const Seperator *sep);
void command_kick(Client *c, const Seperator *sep);
void command_killallnpcs(Client *c, const Seperator *sep);
void command_kill(Client *c, const Seperator *sep);
void command_lastname(Client *c, const Seperator *sep);
void command_level(Client *c, const Seperator *sep);
void command_list(Client *c, const Seperator *sep);
void command_listpetition(Client *c, const Seperator *sep);
void command_lootsim(Client *c, const Seperator *sep);
void command_load_shared_memory(Client *c, const Seperator *sep);
void command_loc(Client *c, const Seperator *sep);
void command_logs(Client *c, const Seperator *sep);
void command_makepet(Client *c, const Seperator *sep);
void command_mana(Client *c, const Seperator *sep);
void command_max_all_skills(Client *c, const Seperator *sep);
void command_memspell(Client *c, const Seperator *sep);
void command_merchantcloseshop(Client *c, const Seperator *sep);
void command_merchantopenshop(Client *c, const Seperator *sep);
void command_modifynpcstat(Client *c, const Seperator *sep);
void command_motd(Client *c, const Seperator *sep);
void command_movechar(Client *c, const Seperator *sep);
void command_movement(Client *c, const Seperator *sep);
void command_myskills(Client *c, const Seperator *sep);
void command_mysql(Client *c, const Seperator *sep);
void command_mystats(Client *c, const Seperator *sep);
void command_name(Client *c, const Seperator *sep);
void command_netstats(Client *c, const Seperator *sep);
void command_network(Client *c, const Seperator *sep);
void command_npccast(Client *c, const Seperator *sep);
void command_npcedit(Client *c, const Seperator *sep);
void command_npceditmass(Client *c, const Seperator *sep);
void command_npcemote(Client *c, const Seperator *sep);
void command_npcloot(Client *c, const Seperator *sep);
void command_npcsay(Client *c, const Seperator *sep);
void command_npcshout(Client *c, const Seperator *sep);
void command_npcspawn(Client *c, const Seperator *sep);
void command_npcstats(Client *c, const Seperator *sep);
void command_npctypespawn(Client *c, const Seperator *sep);
void command_nudge(Client *c, const Seperator *sep);
void command_nukebuffs(Client *c, const Seperator *sep);
void command_nukeitem(Client *c, const Seperator *sep);
void command_object(Client *c, const Seperator *sep);
void command_oocmute(Client *c, const Seperator *sep);
void command_opcode(Client *c, const Seperator *sep);

#ifdef PACKET_PROFILER
void command_packetprofile(Client *c, const Seperator *sep);
#endif

void command_path(Client *c, const Seperator *sep);
void command_peekinv(Client *c, const Seperator *sep);
void command_peqzone(Client *c, const Seperator *sep);
void command_peqzone_flags(Client *c, const Seperator *sep);
void command_permaclass(Client *c, const Seperator *sep);
void command_permagender(Client *c, const Seperator *sep);
void command_permarace(Client *c, const Seperator *sep);
void command_petitems(Client *c, const Seperator *sep);
void command_petitioninfo(Client *c, const Seperator *sep);
void command_picklock(Client *c, const Seperator *sep);
void command_profanity(Client *c, const Seperator *sep);

#ifdef EQPROFILE
void command_profiledump(Client *c, const Seperator *sep);
void command_profilereset(Client *c, const Seperator *sep);
#endif

void command_proximity(Client *c, const Seperator *sep);
void command_push(Client *c, const Seperator *sep);
void command_pvp(Client *c, const Seperator *sep);
void command_qglobal(Client *c, const Seperator *sep);
void command_questerrors(Client *c, const Seperator *sep);
void command_race(Client *c, const Seperator *sep);
void command_raidloot(Client *c, const Seperator *sep);
void command_randomfeatures(Client *c, const Seperator *sep);
void command_refreshgroup(Client *c, const Seperator *sep);
void command_reload(Client *c, const Seperator *sep);
void command_removeitem(Client *c, const Seperator *sep);
void command_repop(Client *c, const Seperator *sep);
void command_resetaa(Client *c, const Seperator *sep);
void command_resetaa_timer(Client *c, const Seperator *sep);
void command_resetdisc_timer(Client *c, const Seperator *sep);
void command_revoke(Client *c, const Seperator *sep);
void command_roambox(Client *c, const Seperator *sep);
void command_rules(Client *c, const Seperator *sep);
void command_save(Client *c, const Seperator *sep);
void command_serverlock(Client *c, const Seperator *sep);
void command_scale(Client *c, const Seperator *sep);
void command_scribespell(Client *c, const Seperator *sep);
void command_scribespells(Client *c, const Seperator *sep);
void command_sendzonespawns(Client *c, const Seperator *sep);
void command_sensetrap(Client *c, const Seperator *sep);
void command_serverinfo(Client *c, const Seperator *sep);
void command_serverrules(Client *c, const Seperator *sep);
void command_set_adventure_points(Client *c, const Seperator *sep);
void command_setaapts(Client *c, const Seperator *sep);
void command_setaaxp(Client *c, const Seperator *sep);
void command_setaltcurrency(Client *c, const Seperator *sep);
void command_setanim(Client *c, const Seperator *sep);
void command_setcrystals(Client *c, const Seperator *sep);
void command_setendurance(Client *c, const Seperator *sep);
void command_setfaction(Client *c, const Seperator *sep);
void command_sethp(Client *c, const Seperator *sep);
void command_setlanguage(Client *c, const Seperator *sep);
void command_setlsinfo(Client *c, const Seperator *sep);
void command_setmana(Client *c, const Seperator *sep);
void command_setpass(Client *c, const Seperator *sep);
void command_setpvppoints(Client *c, const Seperator *sep);
void command_setskill(Client *c, const Seperator *sep);
void command_setskillall(Client *c, const Seperator *sep);
void command_setstartzone(Client *c, const Seperator *sep);
void command_setstat(Client *c, const Seperator *sep);
void command_setxp(Client *c, const Seperator *sep);
//Proxeeus
void command_setroambox(Client *c, const Seperator *sep);
//
void command_showbonusstats(Client *c, const Seperator *sep);
void command_showbuffs(Client *c, const Seperator *sep);
void command_shownumhits(Client *c, const Seperator *sep);
void command_shownpcgloballoot(Client *c, const Seperator *sep);
void command_showpetspell(Client *c, const Seperator *sep);
void command_showskills(Client *c, const Seperator *sep);
void command_showspellslist(Client *c, const Seperator *sep);
void command_showstats(Client *c, const Seperator *sep);
void command_showzonegloballoot(Client *c, const Seperator *sep);
void command_showzonepoints(Client *c, const Seperator *sep);
void command_shutdown(Client *c, const Seperator *sep);
void command_spawn(Client *c, const Seperator *sep);
void command_spawneditmass(Client *c, const Seperator *sep);
void command_spawnfix(Client *c, const Seperator *sep);
void command_spawnstatus(Client *c, const Seperator *sep);
void command_spellinfo(Client *c, const Seperator *sep);
void command_stun(Client *c, const Seperator *sep);
void command_summon(Client *c, const Seperator *sep);
void command_summonburiedplayercorpse(Client *c, const Seperator *sep);
void command_summonitem(Client *c, const Seperator *sep);
void command_suspend(Client *c, const Seperator *sep);
void command_task(Client *c, const Seperator *sep);
void command_tempname(Client *c, const Seperator *sep);
void command_petname(Client *c, const Seperator *sep);
void command_texture(Client *c, const Seperator *sep);
void command_time(Client *c, const Seperator *sep);
void command_timers(Client *c, const Seperator *sep);
void command_timezone(Client *c, const Seperator *sep);
void command_title(Client *c, const Seperator *sep);
void command_titlesuffix(Client *c, const Seperator *sep);
void command_traindisc(Client *c, const Seperator *sep);
void command_trapinfo(Client *c, const Seperator *sep);
void command_tune(Client *c, const Seperator *sep);
void command_undye(Client *c, const Seperator *sep);
void command_undyeme(Client *c, const Seperator *sep);
void command_unfreeze(Client *c, const Seperator *sep);
void command_unlock(Client *c, const Seperator *sep);
void command_unmemspell(Client *c, const Seperator *sep);
void command_unmemspells(Client *c, const Seperator *sep);
void command_unscribespell(Client *c, const Seperator *sep);
void command_unscribespells(Client *c, const Seperator *sep);
void command_untraindisc(Client *c, const Seperator *sep);
void command_untraindiscs(Client *c, const Seperator *sep);
void command_updatechecksum(Client* c, const Seperator* sep);
void command_uptime(Client *c, const Seperator *sep);
void command_version(Client *c, const Seperator *sep);
void command_viewcurrencies(Client *c, const Seperator *sep);
void command_viewnpctype(Client *c, const Seperator *sep);
void command_viewpetition(Client *c, const Seperator *sep);
void command_viewrecipe(Client *c, const Seperator *sep);
void command_viewzoneloot(Client *c, const Seperator *sep);
void command_wc(Client *c, const Seperator *sep);
void command_weather(Client *c, const Seperator *sep);
void command_who(Client *c, const Seperator *sep);
void command_worldshutdown(Client *c, const Seperator *sep);
void command_wp(Client *c, const Seperator *sep);
void command_wpadd(Client *c, const Seperator *sep);
void command_wpinfo(Client *c, const Seperator *sep);
void command_worldwide(Client *c, const Seperator *sep);
void command_xtargets(Client *c, const Seperator *sep);
void command_zclip(Client *c, const Seperator *sep);
void command_zcolor(Client *c, const Seperator *sep);
void command_zheader(Client *c, const Seperator *sep);
void command_zone(Client *c, const Seperator *sep);
void command_zone_instance(Client *c, const Seperator *sep);
void command_zonebootup(Client *c, const Seperator *sep);
void command_zonelock(Client *c, const Seperator *sep);
void command_zoneshutdown(Client *c, const Seperator *sep);
void command_zonestatus(Client *c, const Seperator *sep);
void command_zopp(Client *c, const Seperator *sep);
void command_zsafecoords(Client *c, const Seperator *sep);
void command_zsave(Client *c, const Seperator *sep);
void command_zsky(Client *c, const Seperator *sep);
void command_zstats(Client *c, const Seperator *sep);
void command_zunderworld(Client *c, const Seperator *sep);

#ifdef BOTS
#include "bot.h"
void command_bot(Client*c, const Seperator *sep);
#endif

#endif
