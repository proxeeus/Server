#!/usr/bin/perl
#
# pbchat_generate.pl -- generate a large PlayerBot chat response pack.
#
# EMITS SQL. Never connects to a database, never executes anything.
#
#   perl utils/pbchat_generate.pl                 > /dev/null   # writes the .sql
#   perl utils/pbchat_generate.pl --count 10000
#   perl utils/pbchat_generate.pl --stdout | less
#   perl utils/pbchat_generate.pl --sample 40     # eyeball the output, write nothing
#
# WHY IT IS BUILT THIS WAY
#
# Ten thousand hand-written lines is not ten thousand *distinct* lines -- it is
# three hundred lines wearing hats. Decorating one sentence ("aye, " + core +
# " tbh") reads as mechanical within about a minute of play, which is exactly
# the immersion break this system exists to avoid.
#
# So the variety here comes from EverQuest NOUNS instead. A template consumes
# vocabulary slots -- %ZONE%, %ITEM%, %MOB%, %CAMP%, %SPELL% -- and every
# expansion is a sentence a real player might actually have typed, because the
# thing that changes is the subject, not the packaging:
#
#   'wtb %ITEM%, pst'  x  ~70 items   = 70 genuinely different lines
#   'is %MOB% up?'     x  ~45 mobs    = 45 genuinely different lines
#
# Slots are expanded HERE, at generation time, so the database stores literal
# text. Runtime placeholders -- {speaker}, {self}, {zone}, {level}, {class},
# {race}, {target} -- are passed through untouched and are resolved by the
# engine's substitutor when the line is actually spoken.
#
# Output is deterministic: same arguments produce a byte-identical file, so
# regenerating does not churn the migration.

use strict;
use warnings;

my $TARGET   = 10000;
my $OUT_PATH = 'utils/sql/git/bots/optional/2026_09_13_bots_playerbot_chat_10k.sql';
my $to_stdout = 0;
my $sample    = 0;

while (@ARGV) {
	my $a = shift @ARGV;
	if    ($a eq '--count')  { $TARGET    = shift(@ARGV) or die "--count needs a number\n"; }
	elsif ($a eq '--out')    { $OUT_PATH  = shift(@ARGV) or die "--out needs a path\n"; }
	elsif ($a eq '--stdout') { $to_stdout = 1; }
	elsif ($a eq '--sample') { $sample    = shift(@ARGV) || 30; }
	else { die "unknown option: $a\n"; }
}

# Deterministic PRNG. Not for cryptography -- for a reproducible shuffle.
my $SEED = 20260913;
sub rnd { $SEED = ($SEED * 1103515245 + 12345) % 2147483648; return $SEED; }
sub shuffle_det {
	my @a = @{ $_[0] };
	for (my $i = @a - 1; $i > 0; $i--) {
		my $j = rnd() % ($i + 1);
		@a[$i, $j] = @a[$j, $i];
	}
	return \@a;
}

# ---------------------------------------------------------------------------
# VOCABULARY
# ---------------------------------------------------------------------------

my @ZONE = (
	'Befallen','Crushbone','Blackburrow','Unrest','Mistmoore','Lower Guk','Upper Guk',
	'Sol A','Sol B','Permafrost','Najena','Runnyeye','Kedge Keep','Kaesora','Charasis',
	'Sebilis','Karnors Castle','Chardok','Velketors','Kael','Skyshrine','Thurgadin',
	'the Dreadlands','the Burning Woods','the Overthere','Frontier Mountains',
	'Timorous Deep','Cobalt Scar','Sirens Grotto','the Western Wastes','Temple of Veeshan',
	'the Plane of Fear','the Plane of Hate','the Plane of Sky','the Plane of Growth',
	'Nagafens Lair','Soluseks Eye','Highpass','Kithicor','Rivervale','Misty Thicket',
	'Freeport','Qeynos','the Oasis','South Ro','North Ro','the Rathe Mountains',
	'Lake Rathetear','the Feerrott','Cazic Thule','Innothule','Neriak','Nektulos',
	'Lavastorm','Soldungb','Halas','Everfrost','Qeynos Hills','Western Karana',
	'Eastern Karana','North Karana','South Karana','Erudin','Toxxulia','Paineel',
	'the Hole','Kerra Isle','the Ocean of Tears','Butcherblock','Dagnors Cauldron',
	'Kaladim','Felwithe','Greater Faydark','Lesser Faydark','Steamfont','Ak Anon',
	'Castle Mistmoore','the Gorge of King Xorbb','Beholders Maze','Warsliks Woods',
	'the Field of Bone','Cabilis','the Swamp of No Hope','the Emerald Jungle',
	'Trakanons Teeth','City of Mist','Dalnir','Firiona Vie','the Lake of Ill Omen',
	'the Iceclad Ocean','the Great Divide','the Eastern Wastes','Crystal Caverns',
	'the Tower of Frozen Shadow','the Wakening Lands','the Plane of Mischief',
	'Old Sebilis','Veeshans Peak','Howling Stones','Droga','Nurga','Mesa','Chardok B',
	'Torgiran Mines','Grimling Forest','the Commonlands','West Commons','East Commons',
	'Gorge','Splitpaw','Najena','the Estate of Unrest'
);

# No leading articles. Players type "wtb Fungi Tunic pst", not "wtb a Fungi
# Tunic pst", and an embedded article breaks every template that puts a
# quantifier or possessive in front of the slot ("i have two a Fungi Tunic").
my @ITEM = (
	'Fine Steel Long Sword','Rusty Dagger','Bone Chips','Spiderling Silk','Snake Scales',
	'Fire Beetle Eye','Deathfist Belt','Crushbone Belt','Shiny Brass Idol',
	'Jade Inlaid Coffin','Cracked Staff','Journeymans Boots','Fungi Tunic',
	'Flowing Black Silk Sash','Ivory Handled Falchion','Rubicite Armor',
	'Short Sword of the Ykesha','Bone Razor','Blade of Abrogation','Wurmslayer',
	'Mithril Two Handed Sword','Runed Bolster Belt','Gatorscale Leggings',
	'Cobalt Breastplate','Ravenscale Robe','Robe of the Oracle','Circlet of Shadow',
	'Guise of the Deceiver','Crown of King Tranix','Rod of Annihilation',
	'Staff of Temperate Flux','Golden Efreeti Boots','Cloak of Flames',
	'Shiny Metallic Robe','Manastone','Truesilver Bar','Velium Bar','Water Flasks',
	'Bandages','Muffins','Bread Cakes','Jum Jum Berries','Silver Earring','Pearls',
	'Fire Emerald','Black Sapphire','Diamond','Words of Odus','Glowing Black Stone',
	'Gnomish Spanner','Executioners Axe','Barbed Dragonscale Shield',
	'Shield of the Immaculate','Bronze Weapons','Banded Armor','Crafted Armor',
	'Reinforced Mask','Scaled Mask','Batfang Headband','Tolans Darkwood Bracer',
	'Silver Chitin Handwraps','Sarnak Battle Shield','Iksar Berserker Club',
	'Dwarven Work Boots','Hero Bracers','Tarnished Bronze Sword','Rune of Concussion',
	'Spider Silk','Griffon Feathers','Lion Meat','Gnoll Fangs','Orc Scalps',
	'Giant Snake Fangs','Wolf Pelts','Bear Skins','Peridots','Malachite','Opals',
	'Shralok Pack','Golden Sarnak Mask','Mask of Deception','Sarnak Backbone',
	'Blackened Alloy Great Staff','Coldain Velium Ring','Velium Arrows'
);

my @MOB = (
	'Lord Nagafen','Lady Vox','Trakanon','Phinigel Autropos','Fippy Darkpaw',
	'Sir Lucan DLere','the Ghoul Lord','the Frenzied Ghoul','the Froglok King',
	'Lord Grimrot','Venril Sathir','King Tranix','Drelzna','Ambassador DVinn',
	'Talendor','Gorenaire','Severilous','Faydedar','Innoruuk','Cazic Thule',
	'Dread','Fright','Terror','the Maestro of Rancor','the Froglok Shin Lord',
	'a Sand Giant','a Hill Giant','a Griffon','a Sonic Wolf','Emperor Crush',
	'an Ancient Cyclops','the Mino Lord','a Velium Hound','the Dracoliche',
	'Ghoulbane Knight','the Crypt Guardian','a Spectre','a Dark Elf Necromancer',
	'an Orc Centurion','a Kobold Shaman','the Gnoll Champion','a Sarnak Captain',
	'the Undertaker','a Shadowed Man','the Tower Guardian','a Drolvarg Pawbuster',
	'the Ry Gorr Shaman','a Coldain Elder','the Sentry','a Mammoth'
);

my @CAMP = (
	'the frenzy camp','the live side','the dead side','the entrance','the zone line',
	'the tower','the throne room','the courtyard','the moat','the crypt',
	'the second floor','the basement','the bell','the docks','the spires','the ramp',
	'the pit','the library','the arena','the wall','the bridge','the ruins',
	'the tunnel','the well','the king room','the assassin room','the guard camp',
	'the named spawn','the cave','the tree','the back room','the upper ledge',
	'the first floor','the pond','the gate','the barracks','the nest','the pillars'
);

my @SPELL = (
	'Spirit of Wolf','Clarity','Clarity II','Gate','Bind Affinity','Levitate',
	'Invisibility','See Invisible','Superior Healing','Complete Heal','Resurrection',
	'Harvest','Cannibalize','Lull','Mesmerize','Root','Snare','Enduring Breath',
	'Word of Vigor','Skin like Nature','Ice Comet','Lure of Ice','Shadow Step',
	'Gather Shadows','Bind','Sow','Regeneration','Focus of Spirit','Dead Men Floating'
);

my %VOCAB = (ZONE => \@ZONE, ITEM => \@ITEM, MOB => \@MOB, CAMP => \@CAMP, SPELL => \@SPELL);

# Class bitmasks (GetPlayerClassBit): WAR 1 CLR 2 PAL 4 RNG 8 SHD 16 DRU 32
# MNK 64 BRD 128 ROG 256 SHM 512 NEC 1024 WIZ 2048 MAG 4096 ENC 8192
use constant {
	ANY      => 65535,
	TANKS    => 21,     # WAR PAL SHD
	HEALERS  => 546,    # CLR DRU SHM
	CASTERS  => 15906,  # CLR DRU SHM NEC WIZ MAG ENC
	PORTERS  => 2080,   # WIZ DRU
	SOWERS   => 552,    # DRU RNG SHM
	PETS     => 5120,   # MAG NEC
	KNIGHTS  => 20,     # PAL SHD
	EVILCAST => 1040,   # NEC SHD
};

# ---------------------------------------------------------------------------
# TEMPLATES
#
# [ text, quota_weight, class_mask, alignment, tone ]
# Slots: %ZONE% %ITEM% %MOB% %CAMP% %SPELL%
# Runtime vars ({speaker} etc.) pass straight through to the database.
# ---------------------------------------------------------------------------

my %SPEC = (
	market => { quota => 1000, t => [
		['wtb %ITEM%, pst',                                    ANY, 0, 'slang'],
		['wts %ITEM%, make me an offer',                       ANY, 0, 'slang'],
		['wtb %ITEM% paying well',                             ANY, 0, 'slang'],
		['anyone selling %ITEM%?',                             ANY, 0, 'slang'],
		['pc on %ITEM%?',                                      ANY, 0, 'slang'],
		['looking for %ITEM% if anyone has a spare',            ANY, 0, undef],
		['wts %ITEM%, cheap',                                  ANY, 0, 'slang'],
		['i got %ITEM% if anyone wants it',                    ANY, 0, 'slang'],
		['what is %ITEM% going for these days',                ANY, 0, 'slang'],
		['wtt %ITEM% for something useful',                    ANY, 0, 'slang'],
		['saw %ITEM% go for way too much earlier',             ANY, 0, 'slang'],
		['i will never afford %ITEM% at this rate',            ANY, 0, 'slang'],
		['%ITEM% dropped, anyone need it',                     ANY, 0, 'slang'],
		['%ITEM% drops in %ZONE% btw, dont overpay',           ANY, 0, 'slang'],
	]},
	lfg => { quota => 800, t => [
		['lfg for %CAMP%',                                     ANY, 0, 'slang'],
		['lfm %CAMP%, need dps',                               ANY, 0, 'slang'],
		['lfm %CAMP%, need a healer',                          ANY, 0, 'slang'],
		['lfm %CAMP%, need a tank',                            ANY, 0, 'slang'],
		['anyone camping %CAMP%?',                             ANY, 0, 'slang'],
		['level {level} {class} lfg, heading for %CAMP%',      ANY, 0, 'slang'],
		['got a group going to %ZONE%, room for one',          ANY, 0, 'slang'],
		['need one more for %ZONE%',                           ANY, 0, 'slang'],
		['lfg, i can be at %ZONE% in a few',                   ANY, 0, 'slang'],
		['anyone want to duo %CAMP%?',                         ANY, 0, 'slang'],
		['forming up for %MOB%, pst',                          ANY, 0, 'slang'],
		['we need a puller for %CAMP%',                        ANY, 0, 'slang'],
		['group at %CAMP% has a spot open',                    ANY, 0, 'slang'],
		['i can tank %CAMP% if someone heals',                 TANKS, 0, 'slang'],
		['i can heal for %CAMP%, invite me',                   HEALERS, 0, 'slang'],
		['i can pull %CAMP% if you want',                      ANY, 0, 'slang'],
	]},
	zone_intent => { quota => 700, t => [
		['%ZONE%? mind the road',                              ANY, 0, 'slang'],
		['i was just in %ZONE%, it is packed',                 ANY, 0, 'slang'],
		['%ZONE% is rough at your level',                      ANY, 0, undef],
		['give %MOB% my regards',                              ANY, 0, 'joke'],
		['bind before you go anywhere near %ZONE%',            ANY, 0, undef],
		['the run to %ZONE% takes forever',                    ANY, 0, 'slang'],
		['i died twice getting to %ZONE% last night',          ANY, 0, 'slang'],
		['%CAMP% was open last i checked',                     ANY, 0, 'slang'],
		['watch for %MOB% on the way',                         ANY, 0, undef],
		['i can port you toward %ZONE%',                       PORTERS, 0, 'slang'],
		['sow first, %ZONE% is a long walk',                   SOWERS, 0, 'slang'],
		['%ZONE% at night is a different place',               ANY, 0, undef],
	]},
	zone_intent_opener => { quota => 500, t => [
		['heading to %ZONE% shortly if anyone is going',       ANY, 0, 'slang'],
		['anyone want to run to %ZONE% with me',               ANY, 0, 'slang'],
		['making for %CAMP%, wish me luck',                    ANY, 0, 'slang'],
		['off to see if %MOB% is up',                          ANY, 0, 'slang'],
		['going to try %CAMP% solo. this will end badly',      ANY, 0, 'joke'],
		['zoning to %ZONE%, shout if you need me',             ANY, 0, 'slang'],
		['anyone need a port toward %ZONE%',                   PORTERS, 0, 'slang'],
		['moving camp, %CAMP% is picked clean',                ANY, 0, 'slang'],
		['i hear %MOB% drops something worth having',          ANY, 0, 'slang'],
	]},
	smalltalk_opener => { quota => 1000, t => [
		['anyone know if %MOB% is up?',                        ANY, 0, 'slang'],
		['how long is the respawn on %MOB%',                   ANY, 0, 'slang'],
		['is %CAMP% taken?',                                   ANY, 0, 'slang'],
		['anyone been to %ZONE% lately',                       ANY, 0, 'slang'],
		['still waiting on %MOB%',                             ANY, 0, 'slang'],
		['%ITEM% has never dropped for me. not once',          ANY, 0, 'slang'],
		['does %MOB% even drop %ITEM% anymore',                ANY, 0, 'slang'],
		['i have been at %CAMP% for hours',                    ANY, 0, 'slang'],
		['someone tell me %CAMP% is worth it',                 ANY, 0, 'slang'],
		['what level should i be for %ZONE%',                  ANY, 0, 'slang'],
		['anyone got %SPELL% they can throw me',               ANY, 0, 'slang'],
		['i still need %SPELL% and cannot afford it',          ANY, 0, 'slang'],
		['whats the fastest way to %ZONE% from here',          ANY, 0, 'slang'],
		['i left my corpse in %ZONE%. again',                  ANY, 0, 'slang'],
		['is %MOB% soloable at my level',                      ANY, 0, 'slang'],
		['anyone remember where %ITEM% comes from',            ANY, 0, 'slang'],
	]},
	fallback => { quota => 1200, t => [
		['anyone know if %MOB% is up',                         ANY, 0, 'slang'],
		['still no %ITEM%',                                    ANY, 0, 'slang'],
		['%CAMP% is camped ofc',                               ANY, 0, 'slang'],
		['i should be at %CAMP% instead of talking',           ANY, 0, 'slang'],
		['%ZONE% was a mistake',                               ANY, 0, 'joke'],
		['thinking about heading to %ZONE% later',             ANY, 0, 'slang'],
		['%MOB% killed me twice today',                        ANY, 0, 'slang'],
		['i need %SPELL% before i do anything else',           ANY, 0, 'slang'],
		['wut',                                                ANY, 0, 'slang'],
		['no idea what that means',                            ANY, 0, 'slang'],
		['sure',                                               ANY, 0, 'slang'],
		['anyway, %CAMP% anyone?',                             ANY, 0, 'slang'],
		['is that a %ZONE% thing?',                            ANY, 0, 'slang'],
		['have you seen the drop rate on %ITEM%',              ANY, 0, 'joke'],
		['someone was asking about %MOB% earlier',             ANY, 0, 'slang'],
		['my bags are full of %ITEM%',                         ANY, 0, 'slang'],
		['i keep meaning to go back to %ZONE%',                ANY, 0, 'slang'],
		['%MOB% owes me a drop at this point',                 ANY, 0, 'joke'],
		['heard %CAMP% is good xp',                            ANY, 0, 'slang'],
		['nobody ever runs %ZONE% anymore',                    ANY, 0, 'slang'],
		['i have %ITEM% rotting in the bank',                  ANY, 0, 'slang'],
		['was that about %MOB%?',                              ANY, 0, 'slang'],
	]},
	brag => { quota => 700, t => [
		['i soloed %MOB% last night',                          ANY, 0, 'slang'],
		['we cleared %CAMP% in under an hour',                 ANY, 0, 'slang'],
		['got %ITEM% on the first kill',                       ANY, 0, 'slang'],
		['%MOB% never even landed a hit on me',                ANY, 0, 'slang'],
		['dinged in %ZONE%. finally',                          ANY, 0, 'slang'],
		['finally got %ITEM% after all this time',            ANY, 0, 'slang'],
		['held aggro on %MOB% the whole fight',                TANKS, 0, 'slang'],
		['kited %MOB% for twenty minutes',                     SOWERS, 0, 'slang'],
		['my pet tanked %MOB%. i just watched',                PETS, 0, 'joke'],
		['one nuke and %MOB% was done',                        2048, 0, 'joke'],
		['%CAMP% is mine now',                                 ANY, 0, 'slang'],
		['took %MOB% down a level early',                      ANY, 0, 'slang'],
	]},
	complaint => { quota => 700, t => [
		['%MOB% has not spawned in hours',                     ANY, 0, 'slang'],
		['third wipe at %CAMP% tonight',                       ANY, 0, 'slang'],
		['%ITEM% will never drop for me',                      ANY, 0, 'slang'],
		['lost my corpse somewhere in %ZONE%',                 ANY, 0, 'slang'],
		['%CAMP% has been taken all day',                      ANY, 0, 'slang'],
		['the run back from %ZONE% is brutal',                 ANY, 0, 'slang'],
		['got trained at %CAMP% again',                        ANY, 0, 'slang'],
		['%MOB% resists everything i cast',                    CASTERS, 0, 'slang'],
		['i am out of mana and %MOB% is still up',             CASTERS, 0, 'slang'],
		['four hours at %CAMP% for nothing',                   ANY, 0, 'slang'],
		['%ZONE% hates me specifically',                       ANY, 0, 'joke'],
		['whoever pulled %MOB% into camp, thanks',             ANY, 0, 'joke'],
	]},
	buff_request => { quota => 600, t => [
		['i can cast %SPELL% if you need it',                  CASTERS, 0, 'slang'],
		['need %SPELL% before we pull',                        ANY, 0, 'slang'],
		['who has %SPELL%',                                    ANY, 0, 'slang'],
		['%SPELL% please, heading to %ZONE%',                  ANY, 0, 'slang'],
		['i am out of mana, ask me after i med',               CASTERS, 0, 'slang'],
		['sow up, we are going to %ZONE%',                     SOWERS, 0, 'slang'],
		['i can port as far as %ZONE%',                        PORTERS, 0, 'slang'],
		['rez is coming, hold still',                          6, 0, 'slang'],
		['buffed. now do not die in %ZONE%',                   HEALERS, 0, 'slang'],
		['%SPELL% is up if anyone wants it',                   CASTERS, 0, 'slang'],
	]},
	combat_call => { quota => 500, t => [
		['%MOB% incoming',                                     ANY, 0, 'slang'],
		['train from %CAMP%, move',                            ANY, 0, 'slang'],
		['adds at %CAMP%',                                     ANY, 0, 'slang'],
		['%MOB% is on me, need help',                          ANY, 0, 'slang'],
		['pulling %MOB%, get ready',                           ANY, 0, 'slang'],
		['everyone to the zone line',                          ANY, 0, 'slang'],
		['i have %MOB%, someone take the adds',                TANKS, 0, 'slang'],
		['mezzing the adds at %CAMP%',                         8192, 0, 'slang'],
		['snaring %MOB%',                                      SOWERS, 0, 'slang'],
		['%MOB% is running, snare it',                         ANY, 0, 'slang'],
		['clear %CAMP% before the repop',                      ANY, 0, 'slang'],
		['%MOB% is almost down, do not let it flee',           ANY, 0, 'slang'],
		['back off %MOB%, i am rebuilding aggro',              TANKS, 0, 'slang'],
		['too many adds at %CAMP%, we are leaving',            ANY, 0, 'slang'],
	]},
	greeting => { quota => 500, t => [
		['hail {speaker}, you come far?',                      ANY, 0, 'slang'],
		['well met. headed to %ZONE%?',                        ANY, 0, undef],
		['greetings {speaker}',                                ANY, 0, undef],
		['hey. is %CAMP% open?',                               ANY, 0, 'slang'],
		['oh good, another soul in %ZONE%',                    ANY, 0, 'joke'],
		['you look like you just came from %ZONE%',            ANY, 0, 'joke'],
		['hi. do not go to %CAMP% alone',                      ANY, 0, 'slang'],
		['welcome to {zone}, {speaker}',                       ANY, 0, undef],
		['hail. anyone still at %CAMP%?',                      ANY, 0, 'slang'],
		['hey {speaker}, %MOB% up?',                           ANY, 0, 'slang'],
		['good to see someone else in %ZONE%',                 ANY, 0, undef],
		['new around %ZONE%?',                                 ANY, 0, 'slang'],
	]},
	farewell => { quota => 400, t => [
		['safe travels to %ZONE%',                             ANY, 0, undef],
		['good luck at %CAMP%',                                ANY, 0, 'slang'],
		['say hi to %MOB% for me',                             ANY, 0, 'joke'],
		['later. do not die in %ZONE%',                        ANY, 0, 'slang'],
		['i am camping out, long day in %ZONE%',               ANY, 0, 'slang'],
		['off to %CAMP%. wish me luck',                        ANY, 0, 'slang'],
	]},
	generic_ack => { quota => 400, t => [
		['aye, same happened to me in %ZONE%',                 ANY, 0, 'slang'],
		['heard the same about %MOB%',                         ANY, 0, 'slang'],
		['true, %CAMP% is like that',                          ANY, 0, 'slang'],
		['yeah %ITEM% is rare for everyone',                   ANY, 0, 'slang'],
		['fair. %ZONE% is not for everyone',                   ANY, 0, 'slang'],
		['right, %MOB% is a pain',                             ANY, 0, 'slang'],
	]},
	compliment => { quota => 350, t => [
		['nice work on %MOB%',                                 ANY, 0, 'slang'],
		['grats on %ITEM%',                                    ANY, 0, 'slang'],
		['good pull at %CAMP%',                                ANY, 0, 'slang'],
		['well fought. %MOB% is no joke',                      ANY, 0, undef],
		['that was clean. %CAMP% is hard to hold',             ANY, 0, 'slang'],
		['you made %MOB% look easy',                           ANY, 0, 'slang'],
		['best pull i have seen in %ZONE%',                    ANY, 0, 'slang'],
		['thanks for the assist on %MOB%',                     ANY, 0, 'slang'],
	]},
	insult => { quota => 350, t => [
		['big words for someone who died to %MOB%',            ANY, 0, 'joke'],
		['go take that tone to %ZONE% and see how it goes',    ANY, 0, 'joke'],
		['you could not hold %CAMP% for five minutes',         ANY, 0, 'cruel'],
		['i have seen better out of a %ZONE% newbie',          ANY, 0, 'cruel'],
		['keep talking. %MOB% is right behind you',            ANY, 0, 'joke'],
	]},
	status => { quota => 300, t => [
		['brb, banking in %ZONE%',                             ANY, 0, 'slang'],
		['afk a sec, hold %CAMP%',                             ANY, 0, 'slang'],
		['oom, give me a tick',                                CASTERS, 0, 'slang'],
		['omw to %CAMP%',                                      ANY, 0, 'slang'],
		['medding. do not pull %MOB% yet',                     CASTERS, 0, 'slang'],
		['back. what did i miss at %CAMP%',                    ANY, 0, 'slang'],
		['heading back to %ZONE%, give me a minute',           ANY, 0, 'slang'],
		['low mana, do not pull %MOB%',                        CASTERS, 0, 'slang'],
		['bio, hold %CAMP% for me',                            ANY, 0, 'slang'],
	]},
	victory => { quota => 250, t => [
		['%MOB% is down',                                      ANY, 0, 'slang'],
		['that is %MOB% handled',                              ANY, 0, 'slang'],
		['loot it before %MOB% respawns',                      ANY, 0, 'slang'],
		['please let it be %ITEM%',                            ANY, 0, 'slang'],
		['%CAMP% is clear',                                    ANY, 0, 'slang'],
		['that one hit harder than %MOB%',                     ANY, 0, 'slang'],
	]},
	death => { quota => 250, t => [
		['%MOB% got me',                                       ANY, 0, 'slang'],
		['corpse is at %CAMP%',                                ANY, 0, 'slang'],
		['dead. cr from %ZONE%',                               ANY, 0, 'slang'],
		['someone grab my corpse at %CAMP%',                   ANY, 0, 'slang'],
		['bound in %ZONE%, this will take a while',            ANY, 0, 'slang'],
		['%MOB% resisted everything and then killed me',       CASTERS, 0, 'slang'],
	]},
	aggro => { quota => 200, t => [
		['incoming {target}, be ready',                        ANY, 0, 'slang'],
		['{target} on me',                                     ANY, 0, 'slang'],
		['got {target}, someone assist',                       ANY, 0, 'slang'],
		['{target} is mine, watch for adds',                   TANKS, 0, 'slang'],
		['pulling {target} back to %CAMP%',                    ANY, 0, 'slang'],
		['{target} hits hard, heals up',                       ANY, 0, 'slang'],
		['taunting {target} off you',                          TANKS, 0, 'slang'],
		['snaring {target}',                                   SOWERS, 0, 'slang'],
		['here we go',                                         ANY, 0, 'slang'],
		['{target}? in {zone}? seriously?',                    ANY, 0, 'joke'],
		['{target} pulled from %CAMP%',                        ANY, 0, 'slang'],
		['watch the adds, {target} came from %CAMP%',          ANY, 0, 'slang'],
		['{target} again. %CAMP% is cursed',                   ANY, 0, 'joke'],
		['engaging {target} near %CAMP%',                      ANY, 0, 'slang'],
		['{target} is up, everyone on it',                     ANY, 0, 'slang'],
		['do not let {target} run to %CAMP%',                  ANY, 0, 'slang'],
		['{target} incoming from %ZONE% side',                 ANY, 0, 'slang'],
	]},
);

# ---------------------------------------------------------------------------
# EXPANSION
# ---------------------------------------------------------------------------

sub expand {
	my ($text) = @_;
	my @slots = ($text =~ /%([A-Z]+)%/g);

	unless (@slots) {
		return [$text];
	}

	my @out = ($text);
	my %seen_slot;
	for my $slot (@slots) {
		next if $seen_slot{$slot}++;
		die "unknown vocabulary slot %$slot% in: $text\n" unless $VOCAB{$slot};

		my @next;
		for my $partial (@out) {
			for my $word (@{ $VOCAB{$slot} }) {
				my $copy = $partial;
				$copy =~ s/%\Q$slot\E%/$word/g;
				push @next, $copy;
			}
		}
		@out = @next;
	}

	return \@out;
}

my @rows;       # [category, text, weight, class_mask, alignment, tone]
my @problems;
my $total_quota = 0;
$total_quota += $SPEC{$_}{quota} for keys %SPEC;

# Scale every quota so the totals land on --count.
my $scale = $TARGET / $total_quota;

for my $cat (sort keys %SPEC) {
	my $spec  = $SPEC{$cat};
	my $quota = int($spec->{quota} * $scale + 0.5);

	my @pool;
	for my $t (@{ $spec->{t} }) {
		my ($text, $mask, $align, $tone) = @$t;
		for my $line (@{ expand($text) }) {
			push @pool, [$line, $mask, $align, $tone];
		}
	}

	# Deterministic shuffle so a quota smaller than the pool takes a spread of
	# templates rather than the whole of the first one.
	my $shuffled = shuffle_det(\@pool);

	my %seen;
	my $taken = 0;
	for my $p (@$shuffled) {
		last if $taken >= $quota;
		my ($line, $mask, $align, $tone) = @$p;

		next if $seen{$line}++;

		if ($line =~ /[\[\]]/) {
			push @problems, "$cat: brackets in '$line'";
			next;
		}
		if (length($line) > 120) {
			push @problems, sprintf("%s: %d chars in '%s'", $cat, length($line), $line);
			next;
		}

		push @rows, [$cat, $line, 100, $mask, $align, $tone];
		$taken++;
	}

	if ($taken < $quota) {
		push @problems, "$cat: pool exhausted at $taken of $quota (add templates or vocabulary)";
	}
}

if ($sample) {
	my $s = shuffle_det(\@rows);
	printf STDERR "%d rows generated. random sample of %d:\n\n", scalar @rows, $sample;
	for my $r (@$s[0 .. ($sample - 1 < $#$s ? $sample - 1 : $#$s)]) {
		printf STDERR "  [%-18s] %s\n", $r->[0], $r->[1];
	}
	printf STDERR "\n%d problems\n", scalar @problems;
	print STDERR "  $_\n" for @problems[0 .. ($#problems > 9 ? 9 : $#problems)];
	exit 0;
}

sub sql_str {
	my ($s) = @_;
	return 'NULL' unless defined $s && length $s;
	$s =~ s/\\/\\\\/g;
	$s =~ s/'/''/g;
	return "'$s'";
}

my $out;
if ($to_stdout) { $out = \*STDOUT; }
else { open $out, '>', $OUT_PATH or die "cannot write $OUT_PATH: $!\n"; }

my %per_cat;
$per_cat{ $_->[0] }++ for @rows;

print {$out} <<"HEADER";
-- ===========================================================================
-- PlayerBot chat -- bulk response pack (@{[ scalar @rows ]} rows)
--
-- GENERATED by utils/pbchat_generate.pl. Do not hand-edit: regenerate instead.
-- Deterministic, so regenerating with the same arguments produces an identical
-- file and does not churn this migration.
--
-- Variety comes from EverQuest nouns rather than from decorating one sentence:
-- templates consume zone / item / mob / camp / spell vocabulary, expanded at
-- generation time, so each row is a sentence a player might plausibly type.
-- Runtime placeholders ({speaker}, {self}, {zone}, {level}, {class}, {target})
-- are left intact for the engine's substitutor.
--
-- Depends on 2026_09_13_bots_playerbot_chat.sql and
--            2026_09_13_bots_playerbot_chat_expansion.sql.
--
-- AUTHORED HERE, RUN BY THE OPERATOR. After running: #pbchat reload
--
-- SIZING NOTE. Every zone process holds the whole response set in memory
-- (roughly 3-4 MB at this row count) and PickResponse walks one category per
-- listener per message. That is a few hundred microseconds for a busy zone --
-- fine -- but it is not free, and it scales with rows-per-category, not with
-- total rows. If a zone ever feels heavy, thin the largest categories first:
--   SELECT category_id, COUNT(*) FROM playerbot_chat_responses GROUP BY 1
--     ORDER BY 2 DESC;
-- ===========================================================================


-- ---------------------------------------------------------------------------
-- DRY RUN -- read these numbers before running anything below.
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS responses_before FROM playerbot_chat_responses;

-- Every category this file targets must already exist. A zero on any row means
-- the earlier migrations were not run and that slice would be silently dropped.
HEADER

for my $cat (sort keys %per_cat) {
	printf {$out} "SELECT %s AS category, COUNT(*) AS must_be_1 FROM playerbot_chat_categories WHERE name = %s;\n",
		sql_str($cat), sql_str($cat);
}

print {$out} "\n\n-- ---------------------------------------------------------------------------\n";
print {$out} "-- CATEGORY: aggro (script only -- no triggers, scope 0)\n";
print {$out} "--\n";
print {$out} "-- Driven from Player_Bot.lua event_combat via\n";
print {$out} "--   e.self:PlayerBotChatSayNamed(\"aggro\", 8, e.other:GetCleanName())\n";
print {$out} "-- so {target} resolves to the mob that was just engaged. Zero triggers\n";
print {$out} "-- keeps the classifier out of it; scope 0 keeps the spontaneous scheduler\n";
print {$out} "-- out of it. Guarded so a re-run is a no-op.\n";
print {$out} "-- ---------------------------------------------------------------------------\n";
print {$out} <<'AGGRO';
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'aggro', 100, 20000, 10, 0, 'SCRIPT ONLY (no triggers): bot engaged something. Player_Bot.lua event_combat'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'aggro');

AGGRO

print {$out} "\n-- ---------------------------------------------------------------------------\n";
print {$out} "-- CATEGORY IDS\n";
print {$out} "-- ---------------------------------------------------------------------------\n";
for my $cat (sort keys %per_cat) {
	(my $var = $cat) =~ s/[^A-Za-z0-9_]/_/g;
	printf {$out} "SET \@c_%s := (SELECT id FROM playerbot_chat_categories WHERE name = %s);\n",
		$var, sql_str($cat);
}

print {$out} "\n\n-- ---------------------------------------------------------------------------\n";
print {$out} "-- RESPONSES\n";
for my $cat (sort keys %per_cat) {
	printf {$out} "--   %-20s %5d rows\n", $cat, $per_cat{$cat};
}
print {$out} "-- ---------------------------------------------------------------------------\n\n";

my $BATCH = 250;
my $i     = 0;
while ($i < @rows) {
	my $end = $i + $BATCH - 1;
	$end = $#rows if $end > $#rows;

	print {$out} "INSERT INTO playerbot_chat_responses\n";
	print {$out} "  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES\n";

	my @lines;
	for my $r (@rows[$i .. $end]) {
		my ($cat, $text, $weight, $mask, $align, $tone) = @$r;
		(my $var = $cat) =~ s/[^A-Za-z0-9_]/_/g;
		push @lines, sprintf("  (\@c_%s, %s, %d, %d, 65535, %d, 1, 60, %s)",
			$var, sql_str($text), $weight, $mask, $align, sql_str($tone));
	}
	print {$out} join(",\n", @lines), ";\n\n";

	$i = $end + 1;
}

print {$out} <<'FOOTER';
-- ---------------------------------------------------------------------------
-- VERIFICATION
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS responses_after FROM playerbot_chat_responses;

SELECT c.name, COUNT(r.id) AS rows_per_category
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_responses r ON r.category_id = c.id
GROUP BY c.name ORDER BY rows_per_category DESC;

-- Anything listed here would have been dropped by a missing category.
SELECT COUNT(*) AS orphan_responses FROM playerbot_chat_responses r
  LEFT JOIN playerbot_chat_categories c ON c.id = r.category_id WHERE c.id IS NULL;

-- Content rules. All three should return zero rows.
SELECT id, response_text AS contains_brackets
  FROM playerbot_chat_responses WHERE response_text LIKE '%[%';
SELECT id, CHAR_LENGTH(response_text) AS len
  FROM playerbot_chat_responses WHERE CHAR_LENGTH(response_text) > 120;
SELECT id, reply_channel FROM playerbot_chat_responses
  WHERE reply_channel NOT IN (-1, 2, 3, 4, 5, 8);

-- aggro and the other script-only categories MUST have zero triggers.
SELECT c.name, COUNT(t.id) AS trigger_count_must_be_zero
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_triggers t ON t.category_id = c.id
WHERE c.name IN ('victory','death','aggro')
GROUP BY c.name;

-- Then, in game:  #pbchat reload
--                 #pbchat dumpcats
--                 #pbchat stats reset
FOOTER

close $out unless $to_stdout;

unless ($to_stdout) {
	printf STDERR "wrote %s\n  %d rows across %d categories, %d rejected\n",
		$OUT_PATH, scalar @rows, scalar(keys %per_cat), scalar @problems;
	print STDERR "  $_\n" for @problems[0 .. ($#problems > 19 ? 19 : $#problems)];
	print STDERR "Review it, then run it yourself. Nothing was written to the database.\n";
}
