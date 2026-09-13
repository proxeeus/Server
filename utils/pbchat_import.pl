#!/usr/bin/perl
#
# pbchat_import.pl -- bulk-load PlayerBot chat response content from CSV.
#
# This script EMITS SQL.  It never connects to, or executes anything against,
# a database.  Output is a timestamped .sql file under
# utils/sql/git/bots/optional/ with dry-run SELECT COUNT(*) lines above the
# INSERTs, for the operator to review and run.
#
# Usage:
#   perl utils/pbchat_import.pl responses.csv [more.csv ...]
#   perl utils/pbchat_import.pl --out /tmp/my_content.sql responses.csv
#   perl utils/pbchat_import.pl --stdout responses.csv
#
# CSV columns (header row required, order fixed):
#   category_name,response_text,weight,class_mask,race_mask,alignment,
#   level_min,level_max,tone,reply_channel
#
# Empty cells take the schema default: weight 100, masks 65535 (any class /
# any race), alignment 0, levels 1-60, tone NULL, reply_channel -1.
#
# Example:
#   category_name,response_text,weight,class_mask,race_mask,alignment,level_min,level_max,tone,reply_channel
#   greeting,"Hail, {speaker}!",100,,,,1,60,,
#   insult,"You will die alone in a ditch, {speaker}.",100,,,-1,1,60,cruel,
#
# Content rules enforced here (spec section 14.1) -- violations are reported
# and the offending row is skipped, because both produce bugs that look like
# engine faults in game:
#   * no [brackets]      -> RuleB(Chat, AutoInjectSaylinksToSay) turns them
#                           into saylinks no quest handles
#   * <= 120 characters  -> /ooc and /auction ride Trilogy 0x0721, which
#                           truncates; say/shout dodge it via OP_SpecialMesg
#   * reply_channel in (-1, 3, 4, 5, 8)

use strict;
use warnings;

my @VALID_CHANNELS = (-1, 3, 4, 5, 8);
my $MAX_LEN        = 120;

my $out_path;
my $to_stdout = 0;
my @inputs;

while (@ARGV) {
	my $a = shift @ARGV;
	if ($a eq '--out') {
		$out_path = shift @ARGV or die "--out needs a path\n";
	}
	elsif ($a eq '--stdout') {
		$to_stdout = 1;
	}
	elsif ($a =~ /^-/) {
		die "unknown option: $a\n";
	}
	else {
		push @inputs, $a;
	}
}

unless (@inputs) {
	die "usage: perl utils/pbchat_import.pl [--out FILE|--stdout] <file.csv> [...]\n";
}

# Minimal RFC4180-ish splitter: handles quoted fields and doubled quotes.
sub split_csv_line {
	my ($line) = @_;
	my @f;
	my $cur      = '';
	my $in_quote = 0;
	my @c        = split //, $line;

	for (my $i = 0; $i < @c; $i++) {
		my $ch = $c[$i];
		if ($in_quote) {
			if ($ch eq '"') {
				if (defined $c[$i + 1] && $c[$i + 1] eq '"') { $cur .= '"'; $i++; }
				else                                         { $in_quote = 0; }
			}
			else { $cur .= $ch; }
		}
		else {
			if    ($ch eq '"') { $in_quote = 1; }
			elsif ($ch eq ',') { push @f, $cur; $cur = ''; }
			else               { $cur .= $ch; }
		}
	}
	push @f, $cur;

	s/^\s+|\s+$//g for @f;
	return @f;
}

sub sql_str {
	my ($s) = @_;
	return 'NULL' unless defined $s && length $s;
	$s =~ s/\\/\\\\/g;
	$s =~ s/'/''/g;
	return "'$s'";
}

sub num_or {
	my ($v, $default) = @_;
	return $default unless defined $v && length $v;
	die "not a number: '$v'\n" unless $v =~ /^-?\d+$/;
	return $v + 0;
}

my %categories;    # category_name -> 1
my @rows;          # [cat, text, weight, cmask, rmask, align, lmin, lmax, tone, chan]
my @problems;
my $line_total = 0;

for my $path (@inputs) {
	open my $fh, '<', $path or die "cannot open $path: $!\n";

	my $header = <$fh>;
	unless (defined $header) {
		warn "$path: empty file, skipped\n";
		close $fh;
		next;
	}
	$header =~ s/\x{FEFF}//;    # strip BOM if present

	while (my $line = <$fh>) {
		$line =~ s/\r?\n\z//;
		next unless length $line;
		$line_total++;

		my @f = split_csv_line($line);
		unless (@f >= 2) {
			push @problems, "$path:$.: fewer than 2 columns, skipped";
			next;
		}

		my ($cat, $text, $weight, $cmask, $rmask, $align, $lmin, $lmax, $tone, $chan) = @f;

		unless (defined $cat && length $cat) {
			push @problems, "$path:$.: empty category_name, skipped";
			next;
		}
		unless (defined $text && length $text) {
			push @problems, "$path:$.: empty response_text, skipped";
			next;
		}

		if ($text =~ /\[/ || $text =~ /\]/) {
			push @problems, "$path:$.: response contains [brackets] (becomes a dead-end saylink), skipped";
			next;
		}
		if (length($text) > $MAX_LEN) {
			push @problems, sprintf(
				"%s:%d: response is %d chars, over the %d limit for Trilogy 0x0721, skipped",
				$path, $., length($text), $MAX_LEN
			);
			next;
		}

		my @vals;
		eval {
			@vals = (
				num_or($weight, 100),
				num_or($cmask,  65535),
				num_or($rmask,  65535),
				num_or($align,  0),
				num_or($lmin,   1),
				num_or($lmax,   60),
			);
			1;
		} or do {
			my $err = $@ // 'bad number';
			$err =~ s/\s+\z//;
			push @problems, "$path:$.: $err, skipped";
			next;
		};

		my $channel = eval { num_or($chan, -1) };
		if (!defined $channel) {
			push @problems, "$path:$.: reply_channel is not a number, skipped";
			next;
		}
		unless (grep { $_ == $channel } @VALID_CHANNELS) {
			push @problems, "$path:$.: reply_channel $channel is not one of -1/3/4/5/8, skipped";
			next;
		}

		if ($vals[3] !~ /^(-1|0|1)$/) {
			push @problems, "$path:$.: alignment $vals[3] is not -1/0/1, skipped";
			next;
		}

		$categories{$cat} = 1;
		push @rows, [$cat, $text, @vals, (defined $tone && length $tone) ? $tone : undef, $channel];
	}

	close $fh;
}

unless (@rows) {
	print STDERR "no usable rows found.\n";
	print STDERR "  $_\n" for @problems;
	exit 1;
}

my @t = localtime();
my $stamp = sprintf('%04d_%02d_%02d', $t[5] + 1900, $t[4] + 1, $t[3]);

unless ($to_stdout || defined $out_path) {
	$out_path = "utils/sql/git/bots/optional/${stamp}_bots_playerbot_chat_import.sql";
}

my $out;
if ($to_stdout) {
	$out = \*STDOUT;
}
else {
	open $out, '>', $out_path or die "cannot write $out_path: $!\n";
}

print {$out} <<"HEADER";
-- ===========================================================================
-- PlayerBot chat response import
-- Generated by utils/pbchat_import.pl from: @{[ join ', ', @inputs ]}
--
-- REVIEW THIS FILE, THEN RUN IT YOURSELF.  The importer does not touch the
-- database.  Dry-run counts are below; read them before running the INSERTs.
--
-- Rows accepted: @{[ scalar @rows ]}   Rows rejected: @{[ scalar @problems ]}
-- ===========================================================================

HEADER

if (@problems) {
	print {$out} "-- REJECTED ROWS (fix the CSV and re-run if these matter):\n";
	print {$out} "--   $_\n" for @problems;
	print {$out} "\n";
}

print {$out} "-- ---------------------------------------------------------------------------\n";
print {$out} "-- DRY RUN\n";
print {$out} "-- ---------------------------------------------------------------------------\n";
print {$out} "SELECT COUNT(*) AS responses_before_import FROM playerbot_chat_responses;\n\n";

print {$out} "-- Every category named in the CSV must already exist.  A zero here means\n";
print {$out} "-- the import would silently drop those rows -- create the category first.\n";
for my $cat (sort keys %categories) {
	printf {$out} "SELECT %s AS category, COUNT(*) AS exists_in_db FROM playerbot_chat_categories WHERE name = %s;\n",
		sql_str($cat), sql_str($cat);
}
print {$out} "\n";

print {$out} "-- ---------------------------------------------------------------------------\n";
print {$out} "-- IMPORT\n";
print {$out} "-- ---------------------------------------------------------------------------\n";

for my $cat (sort keys %categories) {
	my $var = $cat;
	$var =~ s/[^A-Za-z0-9_]/_/g;
	printf {$out} "SET \@cat_%s := (SELECT id FROM playerbot_chat_categories WHERE name = %s);\n",
		$var, sql_str($cat);
}
print {$out} "\n";

my %by_cat;
push @{ $by_cat{ $_->[0] } }, $_ for @rows;

for my $cat (sort keys %by_cat) {
	my $var = $cat;
	$var =~ s/[^A-Za-z0-9_]/_/g;

	print {$out} "INSERT INTO playerbot_chat_responses\n";
	print {$out} "  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone, reply_channel) VALUES\n";

	my @lines;
	for my $r (@{ $by_cat{$cat} }) {
		my (undef, $text, $weight, $cmask, $rmask, $align, $lmin, $lmax, $tone, $chan) = @$r;
		push @lines, sprintf(
			"  (\@cat_%s, %s, %d, %d, %d, %d, %d, %d, %s, %d)",
			$var, sql_str($text), $weight, $cmask, $rmask, $align, $lmin, $lmax, sql_str($tone), $chan
		);
	}
	print {$out} join(",\n", @lines), ";\n\n";
}

print {$out} "-- ---------------------------------------------------------------------------\n";
print {$out} "-- VERIFICATION\n";
print {$out} "-- ---------------------------------------------------------------------------\n";
print {$out} "SELECT COUNT(*) AS responses_after_import FROM playerbot_chat_responses;\n";
print {$out} "SELECT COUNT(*) AS orphan_responses FROM playerbot_chat_responses r\n";
print {$out} "  LEFT JOIN playerbot_chat_categories c ON c.id = r.category_id WHERE c.id IS NULL;\n";
print {$out} "\n-- Then, in game as a GM: #pbchat reload\n";

close $out unless $to_stdout;

unless ($to_stdout) {
	printf STDERR "wrote %s (%d rows accepted, %d rejected)\n",
		$out_path, scalar @rows, scalar @problems;
	print STDERR "  $_\n" for @problems;
	print STDERR "Review it, then run it yourself.  Nothing was written to the database.\n";
}
