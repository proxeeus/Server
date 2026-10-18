<?php
/*
 * lua_trades_to_sql.php -- converts lua_modules/playerbot_trading.lua into
 * playerbot_trades rows (see zone/playerbot_native.h TradeKind).
 *
 *   php lua_trades_to_sql.php <playerbot_trading.lua> <out.sql>
 *
 * The trading module is regular enough to read arm by arm:
 *   HandleTrade     check_turn_in(e.trade, {item1 = N}) ... GiveCash(0,0,0,VAR)  -> kind 0
 *   HandleSayTrade  findi("<phrase>") ... TakeMoneyFromPP(ConvertToPP(VAR)) ... SummonItem(N) -> kind 1
 *                   findi("<phrase>") ... HasItem(N) ... GiveCash(0,0,0,VAR)               -> kind 2
 * plus the one enchanter service (kind 3), written out below.
 *
 * Prices are resolved from the module's globals. Nine arms name a variable
 * that does not exist (`anient_leather_*`, and three `*_leather_arms_price`
 * where the definition is `*_leather_sleeves_price`); in Lua those paid
 * GiveCash(nil) -> nothing, AFTER RemoveItem had taken the item. They are
 * mapped to the definition they were meant to name. Any other unresolved
 * name is a hard error.
 */

if ($argc < 3) {
	fwrite(STDERR, "usage: php lua_trades_to_sql.php <playerbot_trading.lua> <out.sql>\n");
	exit(2);
}

function fail($m) { fwrite(STDERR, "ERROR: $m\n"); exit(1); }

$src = file_get_contents($argv[1]);
if ($src === false) fail("cannot read {$argv[1]}");

// ---- price globals ----
preg_match_all('/^\s*([a-z0-9_]+_price)\s*=\s*(\d+)\s*;/m', $src, $m, PREG_SET_ORDER);
$prices = [];
foreach ($m as $x) $prices[$x[1]] = (int)$x[2];

$typos = [
	'anient_leather_boots_price'    => 'ancient_leather_boots_price',
	'anient_leather_bracer_price'   => 'ancient_leather_bracer_price',
	'anient_leather_helm_price'     => 'ancient_leather_helm_price',
	'anient_leather_gloves_price'   => 'ancient_leather_gloves_price',
	'anient_leather_legs_price'     => 'ancient_leather_legs_price',
	'anient_leather_arms_price'     => 'ancient_leather_sleeves_price',
	'anient_leather_bp_price'       => 'ancient_leather_bp_price',
	'eroded_leather_arms_price'     => 'eroded_leather_sleeves_price',
	'unadorned_leather_arms_price'  => 'unadorned_leather_sleeves_price',
];

$fixed_typos = [];
function price($name, $prices, $typos, &$fixed_typos, $where) {
	if (isset($prices[$name])) return $prices[$name];
	if (isset($typos[$name]) && isset($prices[$typos[$name]])) {
		$fixed_typos[$name] = $typos[$name];
		return $prices[$typos[$name]];
	}
	fail("price variable '$name' ($where) is not defined");
}

function body_of($src, $fname) {
	$start = strpos($src, "function playerbot_trading.$fname");
	if ($start === false) fail("function $fname not found");
	$next = strpos($src, "\nfunction ", $start + 10);
	return substr($src, $start, ($next === false ? strlen($src) : $next) - $start);
}

$rows = [];

// ---- kind 0: trade-window turn-ins ----
$trade = body_of($src, 'HandleTrade');
preg_match_all('/check_turn_in\(e\.trade,\s*\{item1\s*=\s*(\d+)\}\)\)\s*then\s*--\s*([^\n]*)\n(.*?)(?=elseif|end\s*\n)/s', $trade, $tm, PREG_SET_ORDER);
foreach ($tm as $x) {
	$item = (int)$x[1];
	if ($item === 16503) continue; // the enchanter plat bar: kind 3 below
	if (!preg_match('/GiveCash\(0,0,0,([a-z0-9_]+)\)/', $x[3], $g)) fail("turn-in $item pays nothing recognisable");
	// labels come from the arm's comment, which is wrong for one pelt
	$label = $item === 13750 ? 'LQ Bear Skin' : trim($x[2]);
	$rows[] = ['kind' => 0, 'item' => $item, 'phrase' => '', 'label' => $label, 'plural' => '',
	           'price' => price($g[1], $prices, $typos, $fixed_typos, "turn-in $item"),
	           'cls' => 0, 'lvl' => 0, 'race' => 0, 'reward' => 0, 'spell' => 0, 'msg' => '', 'fail' => ''];
}

// ---- kind 3: the enchanter service, verbatim from HandleTrade ----
if (strpos($trade, '{item1 = 16503}') === false || strpos($trade, 'CastSpell(670') === false || strpos($trade, 'SummonItem(16507)') === false) {
	fail("the enchanted platinum bar block changed; update the kind 3 row by hand");
}
$rows[] = ['kind' => 3, 'item' => 16503, 'phrase' => '', 'label' => 'Bar of Platinum', 'plural' => '', 'price' => 0,
           'cls' => 14, 'lvl' => 34, 'race' => 0, 'reward' => 16507, 'spell' => 670,
           'msg' => "I'll now transform this bar of platinum into an enchanted one!",
           'fail' => "I'm not powerful enough to enchant your object."];

// ---- kinds 1/2: say-driven ----
$say = body_of($src, 'HandleSayTrade');
$arms = preg_split('/\n\s*(?:else)?if\(e\.message:findi\("/', $say);
array_shift($arms); // the function header
foreach ($arms as $arm) {
	$q = strpos($arm, '"');
	$phrase = strtolower(substr($arm, 0, $q));
	$body = substr($arm, $q);

	$cls = preg_match('/GetClass\(\)\s*==\s*(\d+)/', $body, $c) ? (int)$c[1] : 0;
	$lvl = preg_match('/GetLevel\(\)\s*>=\s*(\d+)/', $body, $c) ? (int)$c[1] : 0;
	$race = preg_match('/GetRace\(\)\s*==\s*(\d+)/', $body, $c) ? (int)$c[1] : 0;

	if (preg_match('/TakeMoneyFromPP\(ConvertToPP\(([a-z0-9_]+)\)/', $body, $p)) {
		if (!preg_match('/SummonItem\((\d+)\)/', $body, $s)) fail("buy '$phrase' summons nothing");
		$label  = preg_match('/Enjoy your (.+?) !/', $body, $l) ? $l[1] : preg_replace('/^buy an? /', '', $phrase);
		$plural = preg_match('/platinum ! (.+?) usually go for/', $body, $l) ? $l[1] : '';
		$rows[] = ['kind' => 1, 'item' => (int)$s[1], 'phrase' => $phrase, 'label' => $label, 'plural' => $plural,
		           'price' => price($p[1], $prices, $typos, $fixed_typos, "buy '$phrase'"),
		           'cls' => $cls, 'lvl' => $lvl, 'race' => $race, 'reward' => 0, 'spell' => 0, 'msg' => '', 'fail' => ''];
		continue;
	}
	if (preg_match('/HasItem\((\d+)\)/', $body, $h)) {
		if (!preg_match('/GiveCash\(0,0,0,([a-z0-9_]+)\)/', $body, $g)) fail("multiquest '$phrase' pays nothing recognisable");
		$rows[] = ['kind' => 2, 'item' => (int)$h[1], 'phrase' => $phrase, 'label' => preg_replace('/^multiquest an? /', '', $phrase), 'plural' => '',
		           'price' => price($g[1], $prices, $typos, $fixed_typos, "multiquest '$phrase'"),
		           'cls' => $cls, 'lvl' => $lvl, 'race' => $race, 'reward' => 0, 'spell' => 0, 'msg' => '', 'fail' => ''];
		continue;
	}
	fail("say arm '$phrase' is neither a buy nor a multiquest");
}

// ---- emit ----
function q($s) { return "'" . str_replace(["\\", "'"], ["\\\\", "''"], $s) . "'"; }

$counts = array_count_values(array_map(function ($r) { return $r['kind']; }, $rows));
$sql = [];
$sql[] = "-- GENERATED by utils/scripts/playerbot_native/lua_trades_to_sql.php from";
$sql[] = "-- lua_modules/playerbot_trading.lua: " . count($rows) . " rows (kind 0: " . ($counts[0] ?? 0)
	. ", kind 1: " . ($counts[1] ?? 0) . ", kind 2: " . ($counts[2] ?? 0) . ", kind 3: " . ($counts[3] ?? 0) . ").";
foreach ($fixed_typos as $bad => $good) $sql[] = "--   price fix: $bad (undefined in Lua, paid nothing) -> $good";
$sql[] = "DELETE FROM playerbot_trades;";
$vals = [];
$id = 1;
foreach ($rows as $r) {
	$vals[] = sprintf("(%d, %d, %d, %s, %s, %s, %d, %d, %d, %d, %d, %d, %s, %s)",
		$id++, $r['kind'], $r['item'], q($r['phrase']), q($r['label']), q($r['plural']), $r['price'],
		$r['cls'], $r['lvl'], $r['race'], $r['reward'], $r['spell'], q($r['msg']), q($r['fail']));
}
$sql[] = "INSERT INTO playerbot_trades (id, kind, item_id, phrase, label, plural, price_platinum, req_class, req_min_level, req_race, reward_item_id, spell_id, message, fail_message) VALUES";
$sql[] = implode(",\n", $vals) . ";";
file_put_contents($argv[2], implode("\n", $sql) . "\n");

fwrite(STDOUT, sprintf("ok: %d rows (kind0 %d, kind1 %d, kind2 %d, kind3 %d), %d price typos mapped\n",
	count($rows), $counts[0] ?? 0, $counts[1] ?? 0, $counts[2] ?? 0, $counts[3] ?? 0, count($fixed_typos)));
