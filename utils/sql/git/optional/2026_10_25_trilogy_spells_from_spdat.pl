#!/usr/bin/perl
# =====================================================================
# Generate 2026_10_25_trilogy_spells_from_spdat.sql: spells_new rows set to
# the v29c client's own spell file (spdat.eff, 22 Aug 2001).
#
# Usage (SELECT only against the database; the .sql is run by hand):
#   mysql -N -B -uroot -p<pw> <db> -e "SELECT <cols> FROM spells_new
#       WHERE id BETWEEN 1 AND 2010" > spells_db.tsv      (cols: see @DB_COLS)
#   perl 2026_10_25_trilogy_spells_from_spdat.pl <spdat.eff> spells_db.tsv \
#       [stats] > 2026_10_25_trilogy_spells_from_spdat.sql
#
# The 608-byte record layout is EQClassic's Zone/Include/spdat.h
# SPDat_Spell_Struct, pinned field by field against this table and the
# client's own reads (recovery +0x1b4 at 0x491f5c, recast +0x1b8, resist type
# +0x221); the bytes EQClassic left as spacers were matched to columns by
# correlation over all 1783 records (see the .sql header).
# =====================================================================
use strict;
use warnings;

my ($spdat_path, $db_path, $mode) = @ARGV;
die "usage: $0 <spdat.eff> <spells_db.tsv> [stats]\n" unless $spdat_path && $db_path;
$mode //= 'sql';

open my $sf, '<:raw', $spdat_path or die "$spdat_path: $!";
my $spdat = do { local $/; <$sf> };
close $sf;
die "spdat.eff is not 3000 x 608 bytes\n" unless length($spdat) == 3000 * 608;

# Columns read from the database, in the order of the TSV.
our @DB_COLS = (
	'id', 'name', 'player_1', 'teleport_zone', 'you_cast', 'other_casts', 'cast_on_you', 'cast_on_other', 'spell_fades',
	'range', 'aoerange', 'pushback', 'pushup', 'cast_time', 'recovery_time', 'recast_time',
	'buffdurationformula', 'buffduration', 'AEDuration', 'mana',
	(map { "effect_base_value$_" } 1 .. 12), (map { "effect_limit_value$_" } 1 .. 12), (map { "max$_" } 1 .. 12),
	'icon', 'memicon', (map { "components$_" } 1 .. 4), (map { "component_counts$_" } 1 .. 4),
	(map { "formula$_" } 1 .. 12), 'LightType', 'goodEffect', 'Activated', 'resisttype',
	(map { "effectid$_" } 1 .. 12), 'targettype', 'basediff', 'skill', 'zonetype', 'EnvironmentType', 'TimeOfDay',
	(map { "classes$_" } 1 .. 15), 'CastingAnim', 'TargetAnim', 'TravelType', 'SpellAffectIndex',
);

# The client record, decoded into the same column names.
sub client_record {
	my ($id) = @_;
	my $r = substr($spdat, $id * 608, 608);
	my %c;
	my $z = sub { my ($o, $n) = @_; my $s = unpack("Z$n", substr($r, $o, $n)); return $s };
	$c{name}          = $z->(0, 32);
	return undef if $c{name} eq '';
	$c{player_1}      = $z->(32, 32);
	$c{teleport_zone} = $z->(64, 32);
	$c{you_cast}      = $z->(96, 64);
	$c{other_casts}   = $z->(160, 64);
	$c{cast_on_you}   = $z->(224, 64);
	$c{cast_on_other} = $z->(288, 64);
	$c{spell_fades}   = $z->(352, 64);

	# Floats in the client, ints in spells_new.
	my @f = unpack('f<4', substr($r, 416, 16));
	my $round = sub { my $v = shift; return $v < 0 ? -int(-$v + 0.5) : int($v + 0.5) };
	@c{qw(range aoerange pushback pushup)} = map { $round->($_) } @f;

	@c{qw(cast_time recovery_time recast_time)} = unpack('l<3', substr($r, 432, 12));
	$c{buffdurationformula} = unpack('C', substr($r, 444, 1));
	$c{buffduration}        = unpack('l<', substr($r, 448, 4));
	$c{AEDuration}          = unpack('l<', substr($r, 452, 4));
	$c{mana}                = unpack('s<', substr($r, 456, 2));

	my @base = unpack('s<12', substr($r, 458, 24));
	my @max  = unpack('s<12', substr($r, 482, 24));
	($c{icon}, $c{memicon}) = unpack('s<2', substr($r, 506, 4));
	my @comp  = unpack('s<4', substr($r, 510, 8));
	my @count = unpack('s<4', substr($r, 518, 8));
	my @form  = unpack('C12', substr($r, 530, 12));
	@c{qw(LightType goodEffect Activated resisttype)} = unpack('C4', substr($r, 542, 4));
	my @eff = unpack('C12', substr($r, 546, 12));
	$c{targettype}      = unpack('C', substr($r, 558, 1));
	$c{basediff}        = unpack('c', substr($r, 559, 1));
	$c{skill}           = unpack('C', substr($r, 560, 1));
	$c{zonetype}        = unpack('c', substr($r, 561, 1));
	$c{EnvironmentType} = unpack('C', substr($r, 562, 1));
	$c{TimeOfDay}       = unpack('C', substr($r, 563, 1));
	my @cls = unpack('C15', substr($r, 565, 15));
	@c{qw(CastingAnim TargetAnim TravelType)} = unpack('C3', substr($r, 596, 3));
	$c{SpellAffectIndex} = unpack('C', substr($r, 604, 1));

	for my $i (1 .. 12) {
		$c{"effect_base_value$i"} = $base[$i - 1];
		$c{"max$i"}               = $max[$i - 1];
		$c{"formula$i"}           = $form[$i - 1];
		$c{"effectid$i"}          = $eff[$i - 1];
	}
	for my $i (1 .. 4) {
		$c{"components$i"}       = $comp[$i - 1];
		$c{"component_counts$i"} = $count[$i - 1];
	}
	# 1..60 is the level; 61 is the client's "this class never".
	for my $i (1 .. 15) {
		$c{"classes$i"} = $cls[$i - 1] >= 61 ? 255 : $cls[$i - 1];
	}
	return \%c;
}

my %is_text = map { $_ => 1 } qw(name player_1 teleport_zone you_cast other_casts cast_on_you cast_on_other spell_fades);

sub unescape { my $s = shift; $s =~ s/\\(.)/$1 eq 't' ? "\t" : $1 eq 'n' ? "\n" : $1 eq '0' ? "\0" : $1/ge; return $s }
sub sqlstr   { my $s = shift; $s =~ s/\\/\\\\/g; $s =~ s/'/''/g; return "'$s'" }

open my $tf, '<', $db_path or die "$db_path: $!";
my %db;
while (my $line = <$tf>) {
	chomp $line;
	$line =~ s/\r$//;
	my @v = split /\t/, $line, -1;
	die "row has " . scalar(@v) . " fields, expected " . scalar(@DB_COLS) . "\n" unless @v == @DB_COLS;
	my %row;
	@row{@DB_COLS} = map { $_ eq 'NULL' ? '' : unescape($_) } @v;
	$db{$row{id}} = \%row;
}
close $tf;

my (%changed_by_col, @updates, $client_n, $missing, $renamed, %kept_self_good, %kept_server_key);

# Effects that look up teleport_zone by name in a server table: SE_SummonPet 33,
# SE_EyeOfZomm 67, SE_NecPet 71, SE_SummonBSTPet 106, SE_Familiar 108,
# SE_SummonHorse 113, SE_TemporaryPets 152.  Checked on both versions of the spell.
my %NAME_KEY_EFFECT = map { $_ => 1 } (33, 67, 71, 106, 108, 113, 152);
sub uses_server_name_key {
	my ($c, $d) = @_;
	for my $i (1 .. 12) {
		return 1 if $NAME_KEY_EFFECT{$c->{"effectid$i"}} || $NAME_KEY_EFFECT{$d->{"effectid$i"}};
	}
	return 0;
}
for my $id (1 .. 2010) {
	my $c = client_record($id) or next;
	$client_n++;
	my $d = $db{$id};
	if (!$d) { $missing++; next; }

	my (@set, @was);
	my $change = sub {
		my ($col, $new) = @_;
		my $old = $d->{$col};
		$changed_by_col{$col}++;
		push @set, "`$col` = " . ($is_text{$col} ? sqlstr($new) : $new);
		push @was, $is_text{$col} ? "$col '$old'->'$new'" : "$col $old->$new";
	};
	for my $col (@DB_COLS) {
		next if $col eq 'id' || $col =~ /^effect_limit_value/;
		my ($cv, $dv) = ($c->{$col}, $d->{$col});
		my $differs = $is_text{$col} ? ($cv ne $dv) : ($cv != ($dv eq '' ? 0 : $dv));
		next unless $differs;
		# The one field kept from the server.  v29c flags six self-only proc
		# buffs detrimental (Vampiric Embrace, Divine Might, Shroud of Death and
		# Undeath, Call of Sky and Fire: goodEffect 0, target self); to the
		# client that is only the icon colour, but Mob::SpellOnTarget refuses a
		# detrimental spell on its own caster (SPELL_NO_HOLD), so copying it
		# would stop the buffs landing at all.
		if ($col eq 'goodEffect' && $cv == 0 && $c->{targettype} == 6 && $dv != 0) {
			$kept_self_good{$id} = $c->{name};
			next;
		}
		# On a pet, Eye of Zomm, horse or swarm-pet spell, teleport_zone is not
		# shown by the client: it is the server's key into its own pets / horses /
		# npc_types tables (MakePet, GetPoweredPetEntry).  The client file has
		# Sony's keys there ('Skeleton241'), which this server's pets table does
		# not hold ('skel_pet_44_'): copied, every necromancer and shadow knight
		# skeleton spell lost its pet, and Bot::LoadPet crashed the zone on a bot
		# with a saved one.
		if ($col eq 'teleport_zone' && uses_server_name_key($c, $d)) {
			$kept_server_key{$id} = $c->{name};
			next;
		}
		$change->($col, $cv);
	}
	# The client has no limit field.  A slot that keeps its effect keeps the
	# server's limit; a slot whose effect changes starts clean.
	for my $i (1 .. 12) {
		if ($c->{"effectid$i"} != $d->{"effectid$i"} && $d->{"effect_limit_value$i"} != 0) {
			$change->("effect_limit_value$i", 0);
		}
	}
	next unless @set;
	$renamed++ if $c->{name} ne $d->{name};
	my $note = join('; ', @was);
	$note =~ s/[\r\n]+/ /g;
	push @updates, sprintf("-- %d %s: %s\nUPDATE `spells_new` SET %s WHERE `id` = %d AND `name` = %s;",
		$id, $d->{name}, $note, join(', ', @set), $id, sqlstr($d->{name}));
}

if ($mode eq 'stats') {
	printf "client records 1..2010: %d, missing in db: %d, rows to update: %d (renamed %d)\n",
		$client_n, $missing // 0, scalar(@updates), $renamed // 0;
	printf "goodEffect kept for self-only spells: %s\n", join(', ', map { "$_ $kept_self_good{$_}" } sort { $a <=> $b } keys %kept_self_good);
	printf "teleport_zone kept as the server's pet/table key: %s\n", join(', ', map { "$_ $kept_server_key{$_}" } sort { $a <=> $b } keys %kept_server_key);
	printf "%-22s %5d\n", $_, $changed_by_col{$_} for sort { $changed_by_col{$b} <=> $changed_by_col{$a} } keys %changed_by_col;
	exit 0;
}

my $n_updates = scalar @updates;
my $kept = join(', ', map { "$_ $kept_self_good{$_}" } sort { $a <=> $b } keys %kept_self_good);
my @top = grep { defined } (sort { $changed_by_col{$b} <=> $changed_by_col{$a} } keys %changed_by_col)[0 .. 11];
$renamed //= 0;
my $top = join("\n", map { sprintf('--     %-22s %4d', $_, $changed_by_col{$_}) } @top);

print <<"HEADER";
-- =====================================================================
-- Trilogy (v29c) spells set to the client's own spell file  --  spells_new
-- =====================================================================
--
-- GENERATED by 2026_10_25_trilogy_spells_from_spdat.pl (same folder) from
-- the client's spdat.eff (22 Aug 2001, the day eqgame.exe was built) and the
-- spells_new it was run against.  Do not edit by hand: regenerate.
--
-- Why: the server's spell table is a later PEQ table, not the 2001 one.  The
-- client computes stats, AC, HP, speed and haste from ITS file and the
-- server from this table, so they disagreed for hundreds of spells (bars and
-- the character sheet snapping, buffs and slows landing at modern values,
-- scribing and cast refusals at the level gaps).  Full 2001 fidelity was
-- chosen: every field the client file carries is copied.  Examples:
--     Togor's Insects  slow capped at 30 -> uncapped, falling with level
--     Visions of Grandeur  70% haste + 230 rune -> 58% haste + AGI 40,
--                          DEX 25, ATK 20
--     Frenzy / Fury / Rage  the 2001 STR/AGI buffs
--     Spirit of Wolf  shaman 9, druid 14, ranger 30 (was never for shaman)
--     Complete Healing  range 600 -> 100;  Lay on Hands / Harm Touch 72 min
--     Translocates  single target + group-only beneficial, not type 43
--     518 spells: the 2250 ms placeholder recast removed (the 2.25 s is the
--     spell's RECOVERY, which v29c's spell bar applies after every cast)
--
-- Scope: ids 1..2010 that hold a spell in the client file ($client_n records).
-- Rows updated: $n_updates ($renamed take the client's name: spelling, and
-- the four Fireworks at 30/119/206/214, which this table had reused).  Only
-- columns that differ are set; each UPDATE has an "old -> new" line above it.
-- Most changed columns:
$top
--
-- Layout: EQClassic Zone/Include/spdat.h SPDat_Spell_Struct (608 bytes,
-- record = id), pinned against the client's own reads (recovery +0x1b4 at
-- 0x491f5c, recast +0x1b8, resist type +0x221).  Bytes EQClassic left as
-- spacers were matched to columns over all 1783 records: 506/508 icon/memicon,
-- 542 LightType, 544 Activated, 559 basediff (signed; the client fizzle code
-- reads it), 561 zonetype (signed), 562 EnvironmentType, 563 TimeOfDay,
-- 596-598 CastingAnim/TargetAnim/TravelType, 604 SpellAffectIndex.
-- Floats (range, aoerange, pushback, pushup) are rounded.  Class level 61 is
-- the client's "never" and becomes 255.
--
-- Not from the client (kept as they are):
--   * every column the client file has no field for: ResistDiff, limits,
--     recourse, short_buff_box, the PoP+ fields...  An effect slot whose
--     effect changes has its limit cleared; one that keeps it keeps it.
--   * goodEffect for six self-only proc buffs ($kept):
--     v29c flags them detrimental, which to the client is only the icon
--     colour, but Mob::SpellOnTarget refuses a detrimental spell on its own
--     caster, so they would stop landing.
--   * teleport_zone on pet, Eye of Zomm, horse and swarm-pet spells (effects
--     33/67/71/106/108/113/152): there it is the server's key into its pets /
--     horses / npc tables, which hold PEQ keys ('skel_pet_44_'), not Sony's
--     ('Skeleton241').  Copying them took every skeleton pet away and crashed
--     Bot::LoadPet.
--   * bytes 526-529 (not matched to anything) and ids above 2010.
--
-- SAFE TO RE-RUN: values are absolute and each row is guarded on the name it
-- had when this was generated.  The first run copies spells_new to
-- spells_new_bak_20261025; later runs leave that copy alone.  To go back:
--     DELETE FROM spells_new;  INSERT INTO spells_new SELECT * FROM spells_new_bak_20261025;
-- Spells live in shared memory: restart the server (world runs
-- shared_memory) after applying.
-- =====================================================================

CREATE TABLE IF NOT EXISTS `spells_new_bak_20261025` LIKE `spells_new`;
SET \@bak_rows = (SELECT COUNT(*) FROM `spells_new_bak_20261025`);
INSERT INTO `spells_new_bak_20261025` SELECT * FROM `spells_new` WHERE \@bak_rows = 0;

START TRANSACTION;

HEADER

print "$_\n" for @updates;

print "\nCOMMIT;\n";
