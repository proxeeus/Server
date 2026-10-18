<?php
/*
 * lua_gear_to_sql.php -- converts lua_modules/playerbot_equipment.lua into the
 * playerbot_gear_draws / playerbot_gear_items tables read by
 * zone/playerbot_native.cpp.
 *
 *   php lua_gear_to_sql.php <playerbot_equipment.lua> <out.sql> <out_draws.lua>
 *
 * The Lua is a decision tree, not a list: "roll one-handed; if so roll a
 * shield". So instead of pattern-matching AddItem lines, this script
 * INTERPRETS the module -- a tokenizer, a parser for the subset of Lua the
 * file uses, and a symbolic evaluator -- once per (class, race size), with:
 *
 *   - `class` and `race_small/medium/large` concrete,
 *   - `level` symbolic: a condition on it narrows an interval instead of
 *     choosing a branch, so one pass covers levels 1..255,
 *   - every eq.ChooseRandom(true/false, ...) a FLAG: a condition draw with a
 *     chance, which later draws gate on (hit / miss),
 *   - every eq.ChooseRandom(items/pickers...) a POOL with exact rational
 *     weights (a nested picker shares its parent's slot evenly).
 *
 * Each executed AddItem becomes a draw; identical draws across classes and
 * sizes are merged into one row with class/size bitmasks. Draw order is the
 * Lua execution order (call-stack position), which matters: NPC::AddItem
 * auto-slots, so the first one-hander lands in Primary.
 *
 * Empty pickers: 32 functions are an empty eq.ChooseRandom(). Under LuaJIT
 * (what zone.exe embeds) math.random(0) does not raise, so they return nil,
 * and since every one of them is an argument to another ChooseRandom, the nil
 * just drops out of {...}. This script gives them exactly that meaning:
 * dropped from the parent pool. An empty pool fed straight to AddItem would
 * have raised in luabind; none exists, and the run reports the count.
 * validate_gear.lua runs the ORIGINAL Lua and compares distributions against
 * these rows.
 *
 * Anything outside the supported subset is a hard error, never a guess.
 */

error_reporting(E_ALL);

if ($argc < 4) {
	fwrite(STDERR, "usage: php lua_gear_to_sql.php <playerbot_equipment.lua> <out.sql> <out_draws.lua>\n");
	exit(2);
}

$src_path = $argv[1];
$out_sql  = $argv[2];
$out_lua  = $argv[3];

const LEVEL_MIN = 1;
const LEVEL_MAX = 255;

function fail($msg) {
	fwrite(STDERR, "ERROR: $msg\n");
	exit(1);
}

// ---------------------------------------------------------------------------
// Rationals (exact pool weights)
// ---------------------------------------------------------------------------

function gcd($a, $b) { $a = abs($a); $b = abs($b); while ($b) { [$a, $b] = [$b, $a % $b]; } return $a ?: 1; }
function rat($n, $d = 1) { if ($d == 0) fail("rational /0"); $g = gcd($n, $d); if ($d < 0) { $n = -$n; $d = -$d; } return [intdiv($n, $g), intdiv($d, $g)]; }
function rat_add($a, $b) { return rat($a[0] * $b[1] + $b[0] * $a[1], $a[1] * $b[1]); }
function rat_mul($a, $b) { return rat($a[0] * $b[0], $a[1] * $b[1]); }

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

function tokenize($s) {
	$toks = [];
	$i = 0; $n = strlen($s); $line = 1;
	while ($i < $n) {
		$c = $s[$i];
		if ($c === "\n") { $line++; $i++; continue; }
		if (ctype_space($c)) { $i++; continue; }
		if (substr($s, $i, 2) === '--') {
			if (substr($s, $i + 2, 2) === '[[') {
				$end = strpos($s, ']]', $i + 4);
				if ($end === false) fail("unterminated block comment at line $line");
				$line += substr_count(substr($s, $i, $end - $i), "\n");
				$i = $end + 2;
			} else {
				while ($i < $n && $s[$i] !== "\n") $i++;
			}
			continue;
		}
		if (ctype_alpha($c) || $c === '_') {
			$j = $i;
			while ($j < $n && (ctype_alnum($s[$j]) || $s[$j] === '_')) $j++;
			$w = substr($s, $i, $j - $i);
			$kw = ['function','local','if','then','elseif','else','end','return','and','or','not','true','false','nil'];
			$toks[] = [in_array($w, $kw, true) ? 'kw' : 'name', $w, $line];
			$i = $j; continue;
		}
		if (ctype_digit($c)) {
			$j = $i;
			while ($j < $n && (ctype_digit($s[$j]) || $s[$j] === '.')) $j++;
			$toks[] = ['num', substr($s, $i, $j - $i) + 0, $line];
			$i = $j; continue;
		}
		if ($c === '"' || $c === "'") {
			$j = $i + 1;
			while ($j < $n && $s[$j] !== $c) { if ($s[$j] === '\\') $j++; $j++; }
			$toks[] = ['str', substr($s, $i + 1, $j - $i - 1), $line];
			$i = $j + 1; continue;
		}
		$two = substr($s, $i, 2);
		if (in_array($two, ['==', '>=', '<=', '~=', '..'], true)) { $toks[] = ['op', $two, $line]; $i += 2; continue; }
		if (strpos('(),.:=<>{}[];+-*/#', $c) !== false) { $toks[] = ['op', $c, $line]; $i++; continue; }
		fail("unexpected character '$c' at line $line");
	}
	$toks[] = ['eof', null, $line];
	return $toks;
}

// ---------------------------------------------------------------------------
// Parser (the subset playerbot_equipment.lua uses)
// ---------------------------------------------------------------------------

class Parser {
	public $t; public $p = 0;
	function __construct($toks) { $this->t = $toks; }
	function peek($k = 0) { return $this->t[$this->p + $k]; }
	function next() { return $this->t[$this->p++]; }
	function is($type, $val = null) { $x = $this->peek(); return $x[0] === $type && ($val === null || $x[1] === $val); }
	function accept($type, $val = null) { if ($this->is($type, $val)) return $this->next(); return null; }
	function expect($type, $val = null) {
		$x = $this->next();
		if ($x[0] !== $type || ($val !== null && $x[1] !== $val)) fail("expected $type " . ($val ?? '') . " got {$x[0]} '{$x[1]}' at line {$x[2]}");
		return $x;
	}

	function block($terminators) {
		$stmts = [];
		while (true) {
			$x = $this->peek();
			if ($x[0] === 'eof' || ($x[0] === 'kw' && in_array($x[1], $terminators, true))) return $stmts;
			if ($this->accept('op', ';')) continue;
			$stmts[] = $this->statement();
		}
	}

	function statement() {
		$x = $this->peek();
		$line = $x[2];
		if ($this->accept('kw', 'function')) {
			$name = $this->expect('name')[1];
			while ($this->accept('op', '.') || $this->accept('op', ':')) $name .= '.' . $this->expect('name')[1];
			return ['func', $name, $this->funcbody(), $line];
		}
		if ($this->accept('kw', 'local')) {
			if ($this->accept('kw', 'function')) {
				$name = $this->expect('name')[1];
				return ['func', $name, $this->funcbody(), $line];
			}
			$name = $this->expect('name')[1];
			$expr = $this->accept('op', '=') ? $this->expr() : ['nil'];
			return ['local', $name, $expr, $line];
		}
		if ($this->accept('kw', 'if')) {
			$arms = [];
			$cond = $this->expr(); $this->expect('kw', 'then');
			$arms[] = [$cond, $this->block(['elseif', 'else', 'end'])];
			$else = null;
			while (true) {
				if ($this->accept('kw', 'elseif')) {
					$cond = $this->expr(); $this->expect('kw', 'then');
					$arms[] = [$cond, $this->block(['elseif', 'else', 'end'])];
					continue;
				}
				if ($this->accept('kw', 'else')) { $else = $this->block(['end']); }
				$this->expect('kw', 'end');
				break;
			}
			return ['if', $arms, $else, $line];
		}
		if ($this->accept('kw', 'return')) {
			if ($this->is('kw', 'end') || $this->is('kw', 'else') || $this->is('kw', 'elseif') || $this->is('eof')) return ['return', null, $line];
			return ['return', $this->expr(), $line];
		}
		// assignment or call
		$e = $this->suffixed();
		if ($this->accept('op', '=')) {
			if ($e[0] !== 'var') fail("unsupported assignment target at line $line");
			return ['assign', $e[1], $this->expr(), $line];
		}
		if ($e[0] !== 'call' && $e[0] !== 'mcall') fail("expression statement is not a call at line $line");
		return ['call', $e, $line];
	}

	function funcbody() {
		$this->expect('op', '(');
		$params = [];
		if (!$this->is('op', ')')) {
			do { $params[] = $this->expect('name')[1]; } while ($this->accept('op', ','));
		}
		$this->expect('op', ')');
		$body = $this->block(['end']);
		$this->expect('kw', 'end');
		return [$params, $body];
	}

	function expr() { return $this->orexpr(); }
	function orexpr() { $l = $this->andexpr(); while ($this->accept('kw', 'or')) $l = ['or', $l, $this->andexpr()]; return $l; }
	function andexpr() { $l = $this->cmpexpr(); while ($this->accept('kw', 'and')) $l = ['and', $l, $this->cmpexpr()]; return $l; }
	function cmpexpr() {
		$l = $this->unary();
		foreach (['==', '~=', '>=', '<=', '<', '>'] as $op) {
			if ($this->accept('op', $op)) return ['cmp', $op, $l, $this->unary()];
		}
		return $l;
	}
	function unary() {
		if ($this->accept('kw', 'not')) return ['not', $this->unary()];
		return $this->primary();
	}
	function primary() {
		$x = $this->peek();
		if ($x[0] === 'num') { $this->next(); return ['num', $x[1]]; }
		if ($x[0] === 'str') { $this->next(); return ['str', $x[1]]; }
		if ($this->accept('kw', 'true')) return ['bool', true];
		if ($this->accept('kw', 'false')) return ['bool', false];
		if ($this->accept('kw', 'nil')) return ['nil'];
		if ($this->accept('op', '{')) { $this->expect('op', '}'); return ['table']; }
		return $this->suffixed();
	}
	function suffixed() {
		$x = $this->peek();
		if ($this->accept('op', '(')) { $e = $this->expr(); $this->expect('op', ')'); }
		else { $e = ['var', $this->expect('name')[1]]; }
		while (true) {
			if ($this->accept('op', '.')) {
				if ($e[0] !== 'var') fail("field access on non-name at line {$x[2]}");
				$e = ['var', $e[1] . '.' . $this->expect('name')[1]];
				continue;
			}
			if ($this->accept('op', ':')) {
				$name_tok = $this->expect('name');
				$e = ['mcall', $e, $name_tok[1], $this->args(), $name_tok[2]];
				continue;
			}
			if ($this->is('op', '(')) {
				// the line of the '(' -- NOT of whatever follows the arguments,
				// which can be the next statement and would misorder draws
				$open_line = $this->peek()[2];
				$e = ['call', $e, $this->args(), $open_line];
				continue;
			}
			return $e;
		}
	}
	function args() {
		$this->expect('op', '(');
		$a = [];
		if (!$this->is('op', ')')) {
			do { $a[] = $this->expr(); } while ($this->accept('op', ','));
		}
		$this->expect('op', ')');
		return $a;
	}
}

// ---------------------------------------------------------------------------
// Symbolic interpreter
// ---------------------------------------------------------------------------
//
// Values:   ['num', n] ['bool', b] ['nil'] ['str', s] ['table']
//           ['pool', [item_id => rat]]   (empty pool = an empty ChooseRandom)
//           ['flag', ref]                (a condition draw)
//           ['level']                    (the symbolic bot level)
// Conds:    ['const', b] ['level', lo, hi] ['gate', ref, hit]
// Context:  ['lo' => , 'hi' => , 'gate' => null|[ref, hit]]

class Interp {
	public $funcs = [];
	public $globals = [];
	public $draws = [];     // raw draws for this run, in execution order
	public $flags = [];     // ref => ['gate' => ctx gate, 'chance' => int]
	public $next_ref = 0;
	public $stack = [];     // call-site lines, for ordering + identity

	function lookup(&$frames, $name) {
		for ($i = count($frames) - 1; $i >= 0; --$i) {
			if (array_key_exists($name, $frames[$i])) return $frames[$i][$name];
		}
		if (array_key_exists($name, $this->globals)) return $this->globals[$name];
		return ['nil'];
	}
	function store(&$frames, $name, $val, $ctx) {
		for ($i = count($frames) - 1; $i >= 0; --$i) {
			if (array_key_exists($name, $frames[$i])) { $frames[$i][$name] = $this->tagged($val, $ctx); return; }
		}
		$this->globals[$name] = $this->tagged($val, $ctx);
	}
	// A variable assigned under a non-trivial context cannot be read back
	// outside it without path merging; tag it so a later read fails loudly.
	function tagged($val, $ctx) {
		$val['ctx'] = [$ctx['lo'], $ctx['hi'], $ctx['gate']];
		return $val;
	}
	function check_read($val, $ctx, $name, $line) {
		if (!isset($val['ctx'])) return $val;
		[$lo, $hi, $gate] = $val['ctx'];
		$inside = $ctx['lo'] >= $lo && $ctx['hi'] <= $hi && $this->gate_implies($ctx['gate'], $gate);
		if (!$inside && in_array($val[0], ['num', 'bool', 'pool'], true)) {
			fail("variable '$name' read at line $line outside the branch that assigned it");
		}
		return $val;
	}

	// does gate context A imply gate context B?
	function gate_implies($a, $b) {
		if ($b === null) return true;
		while ($a !== null) {
			if ($a[0] === $b[0] && $a[1] === $b[1]) return true;
			$a = $this->flags[$a[0]]['gate'];
		}
		return false;
	}

	function call_func($name, $argvals, $ctx, $line) {
		if (!isset($this->funcs[$name])) fail("call to unknown function '$name' at line $line");
		[$params, $body] = $this->funcs[$name];
		$frame = [];
		foreach ($params as $i => $p) $frame[$p] = $argvals[$i] ?? ['nil'];
		$frames = [$frame];
		$this->stack[] = $line;
		$r = $this->exec_block($body, $frames, $ctx);
		array_pop($this->stack);
		return $r === null ? ['nil'] : $r;
	}

	function exec_block($stmts, &$frames, $ctx) {
		$frames[] = [];
		foreach ($stmts as $s) {
			$r = $this->exec($s, $frames, $ctx);
			if ($r !== null) { array_pop($frames); return $r; }
		}
		array_pop($frames);
		return null;
	}

	// returns null, or a return value (only allowed on an unconditional path)
	function exec($s, &$frames, $ctx) {
		switch ($s[0]) {
			case 'func':
				$this->funcs[$s[1]] = $s[2];
				return null;
			case 'local':
				$frames[count($frames) - 1][$s[1]] = $this->tagged($this->eval($s[2], $frames, $ctx, $s[3]), $ctx);
				return null;
			case 'assign':
				$this->store($frames, $s[1], $this->eval($s[2], $frames, $ctx, $s[3]), $ctx);
				return null;
			case 'call':
				$this->eval($s[1], $frames, $ctx, $s[2]);
				return null;
			case 'return':
				return $s[1] === null ? ['nil'] : $this->eval($s[1], $frames, $ctx, $s[2]);
			case 'if':
				return $this->exec_if($s, $frames, $ctx);
		}
		fail("unknown statement {$s[0]}");
	}

	function exec_if($s, &$frames, $ctx) {
		[$_, $arms, $else, $line] = $s;
		$rem = $ctx;
		foreach ($arms as [$cond_expr, $body]) {
			$c = $this->cond($cond_expr, $frames, $rem, $line);
			if ($c[0] === 'const') {
				if ($c[1]) return $this->branch($body, $frames, $rem, $ctx, $line);
				continue;
			}
			if ($c[0] === 'level') {
				$then = $rem;
				$then['lo'] = max($rem['lo'], $c[1]);
				$then['hi'] = min($rem['hi'], $c[2]);
				if ($then['lo'] <= $then['hi']) {
					$r = $this->branch($body, $frames, $then, $ctx, $line);
					if ($r !== null) fail("return under a level condition at line $line");
				}
				// remaining = rem minus [c1, c2]; must stay one interval
				$left  = [$rem['lo'], min($rem['hi'], $c[1] - 1)];
				$right = [max($rem['lo'], $c[2] + 1), $rem['hi']];
				$has_left = $left[0] <= $left[1];
				$has_right = $right[0] <= $right[1];
				if ($has_left && $has_right) fail("level condition at line $line splits the remaining range in two");
				if (!$has_left && !$has_right) return null;
				[$rem['lo'], $rem['hi']] = $has_left ? $left : $right;
				continue;
			}
			if ($c[0] === 'gate') {
				$then = $rem;
				$then['gate'] = $this->combine_gate($rem['gate'], [$c[1], $c[2]], $line);
				$r = $this->branch($body, $frames, $then, $ctx, $line);
				if ($r !== null) fail("return under a flag condition at line $line");
				// remaining = the opposite outcome of the same flag
				$rem['gate'] = $this->combine_gate($rem['gate'], [$c[1], !$c[2]], $line, true);
				if ($rem['gate'] === false) return null;
				continue;
			}
			fail("unsupported condition kind at line $line");
		}
		if ($else !== null) {
			return $this->branch($else, $frames, $rem, $ctx, $line);
		}
		return null;
	}

	// A branch narrows the context; a `return` is only legal when it did not.
	function branch($body, &$frames, $inner, $outer, $line) {
		$r = $this->exec_block($body, $frames, $inner);
		if ($r !== null && $inner != $outer) fail("conditional return at line $line");
		return $r;
	}

	// Entering a gate inside an existing gate context: the new flag must have
	// been created under the current context (so its outcome already implies
	// it). $allow_contradiction: for "the remaining arms", a contradiction
	// means no arm can run -> false.
	function combine_gate($cur, $new, $line, $allow_contradiction = false) {
		if ($cur === null) return $new;
		if ($cur[0] === $new[0]) {
			if ($cur[1] === $new[1]) return $cur;
			if ($allow_contradiction) return false;
			fail("contradictory flag condition at line $line");
		}
		if ($this->gate_implies([$new[0], $new[1]], $cur)) return $new;
		fail("flag condition at line $line needs a conjunction of two independent flags (unsupported)");
	}

	function cond($e, &$frames, $ctx, $line) {
		switch ($e[0]) {
			case 'or':
			case 'and':
				$a = $this->cond($e[1], $frames, $ctx, $line);
				$b = $this->cond($e[2], $frames, $ctx, $line);
				$is_or = $e[0] === 'or';
				if ($a[0] === 'const') return ($a[1] === $is_or) ? $a : $b;
				if ($b[0] === 'const') return ($b[1] === $is_or) ? $b : $a;
				if (!$is_or && $a[0] === 'level' && $b[0] === 'level') return ['level', max($a[1], $b[1]), min($a[2], $b[2])];
				fail("unsupported '{$e[0]}' of {$a[0]} and {$b[0]} at line $line");
			case 'not':
				$a = $this->cond($e[1], $frames, $ctx, $line);
				if ($a[0] === 'const') return ['const', !$a[1]];
				if ($a[0] === 'gate') return ['gate', $a[1], !$a[2]];
				fail("unsupported 'not' at line $line");
			case 'cmp':
				[$_, $op, $l, $r] = $e;
				$lv = $this->eval($l, $frames, $ctx, $line);
				$rv = $this->eval($r, $frames, $ctx, $line);
				if ($lv[0] === 'level' || $rv[0] === 'level') {
					if ($rv[0] === 'level') { [$lv, $rv] = [$rv, $lv]; $op = ['>=' => '<=', '<=' => '>=', '>' => '<', '<' => '>', '==' => '=='][$op] ?? $op; }
					if ($rv[0] !== 'num') fail("level compared with non-number at line $line");
					$n = (int)$rv[1];
					switch ($op) {
						case '>=': return ['level', $n, LEVEL_MAX];
						case '>':  return ['level', $n + 1, LEVEL_MAX];
						case '<=': return ['level', LEVEL_MIN, $n];
						case '<':  return ['level', LEVEL_MIN, $n - 1];
						case '==': return ['level', $n, $n];
					}
					fail("unsupported level comparison '$op' at line $line");
				}
				if ($lv[0] === 'flag' || $rv[0] === 'flag') {
					if ($rv[0] === 'flag') [$lv, $rv] = [$rv, $lv];
					if ($rv[0] !== 'bool' || ($op !== '==' && $op !== '~=')) fail("unsupported flag comparison at line $line");
					$hit = ($rv[1] === true) === ($op === '==');
					return ['gate', $lv[1], $hit];
				}
				$a = $this->scalar($lv, $line); $b = $this->scalar($rv, $line);
				switch ($op) {
					case '==': return ['const', $a === $b];
					case '~=': return ['const', $a !== $b];
					case '>=': return ['const', $a >= $b];
					case '<=': return ['const', $a <= $b];
					case '>':  return ['const', $a > $b];
					case '<':  return ['const', $a < $b];
				}
				fail("unsupported comparison at line $line");
			default:
				$v = $this->eval($e, $frames, $ctx, $line);
				if ($v[0] === 'flag') return ['gate', $v[1], true];
				if ($v[0] === 'bool') return ['const', $v[1]];
				if ($v[0] === 'nil') return ['const', false];
				return ['const', true];
		}
	}

	function scalar($v, $line) {
		if ($v[0] === 'num') return 0 + $v[1];
		if ($v[0] === 'bool') return $v[1];
		if ($v[0] === 'nil') return null;
		if ($v[0] === 'str') return $v[1];
		fail("unsupported value {$v[0]} in comparison at line $line");
	}

	function eval($e, &$frames, $ctx, $line) {
		switch ($e[0]) {
			case 'num': case 'bool': case 'nil': case 'str': case 'table':
				return $e;
			case 'var':
				return $this->check_read($this->lookup($frames, $e[1]), $ctx, $e[1], $line);
			case 'call':
				[$_, $fe, $args, $cline] = $e;
				if ($fe[0] !== 'var') fail("call of non-name at line $cline");
				$name = $fe[1];
				$argv = [];
				foreach ($args as $a) $argv[] = $this->eval($a, $frames, $ctx, $cline);
				if ($name === 'eq.ChooseRandom') return $this->choose_random($argv, $ctx, $cline);
				if ($name === 'require') return ['table'];
				return $this->call_func($name, $argv, $ctx, $cline);
			case 'mcall':
				[$_, $obj, $m, $args, $cline] = $e;
				if ($m !== 'AddItem') fail("unsupported method call :$m at line $cline");
				$item = $this->eval($args[0], $frames, $ctx, $cline);
				$this->add_item($item, $ctx, $cline);
				return ['nil'];
			case 'cmp': case 'and': case 'or': case 'not':
				$c = $this->cond($e, $frames, $ctx, $line);
				if ($c[0] === 'const') return ['bool', $c[1]];
				fail("symbolic boolean used as a value at line $line");
		}
		fail("unsupported expression {$e[0]} at line $line");
	}

	function choose_random($argv, $ctx, $line) {
		$bools = 0; $trues = 0; $other = 0;
		foreach ($argv as $v) {
			if ($v[0] === 'bool') { $bools++; if ($v[1]) $trues++; }
			else $other++;
		}
		if ($bools && $other) fail("eq.ChooseRandom mixes booleans and items at line $line");

		if ($bools) {
			if ($trues === 0) return ['bool', false];
			if ($trues === $bools) return ['bool', true];
			$chance = 100 * $trues / $bools;
			if ($chance != (int)$chance) fail("flag chance $chance% is not an integer at line $line");
			$ref = $this->next_ref++;
			$this->flags[$ref] = ['gate' => $ctx['gate'], 'chance' => (int)$chance];
			$this->draws[] = [
				'kind' => 'flag', 'ref' => $ref, 'pos' => array_merge($this->stack, [$line]),
				'lo' => $ctx['lo'], 'hi' => $ctx['hi'], 'gate' => $ctx['gate'], 'chance' => (int)$chance, 'pool' => [],
			];
			return ['flag', $ref];
		}

		// Item pool. Fixed-port: an empty pool argument (empty ChooseRandom)
		// is dropped, exactly as a nil vararg vanishes from {...} in Lua.
		$parts = [];
		foreach ($argv as $v) {
			if ($v[0] === 'num') $parts[] = [(int)$v[1] => rat(1)];
			elseif ($v[0] === 'pool') { if ($v[1]) $parts[] = $v[1]; }
			elseif ($v[0] === 'nil') { /* dropped */ }
			else fail("eq.ChooseRandom argument {$v[0]} at line $line");
		}
		$pool = [];
		$k = count($parts);
		foreach ($parts as $p) {
			foreach ($p as $id => $w) {
				$share = rat_mul($w, rat(1, $k));
				$pool[$id] = isset($pool[$id]) ? rat_add($pool[$id], $share) : $share;
			}
		}
		return ['pool', $pool];
	}

	function add_item($v, $ctx, $line) {
		if ($v[0] === 'num') $v = ['pool', [(int)$v[1] => rat(1)]];
		if ($v[0] === 'nil') $v = ['pool', []];
		if ($v[0] !== 'pool') fail("AddItem of {$v[0]} at line $line");
		$this->draws[] = [
			'kind' => 'item', 'pos' => array_merge($this->stack, [$line]),
			'lo' => $ctx['lo'], 'hi' => $ctx['hi'], 'gate' => $ctx['gate'], 'chance' => 100, 'pool' => $v[1],
		];
	}
}

// ---------------------------------------------------------------------------
// Drive: one interpretation per (class, race size)
// ---------------------------------------------------------------------------

$src = file_get_contents($src_path);
if ($src === false) fail("cannot read $src_path");
$parser = new Parser(tokenize($src));
$chunk = $parser->block([]);

$sizes = [1 => 'small', 2 => 'medium', 4 => 'large'];
$runs = [];
$empty_item_draws = 0;

foreach (range(1, 14) as $class) {
	foreach ($sizes as $size_bit => $size_name) {
		$I = new Interp();
		$frames = [[]];
		$top = ['lo' => LEVEL_MIN, 'hi' => LEVEL_MAX, 'gate' => null];
		foreach ($chunk as $s) {
			if ($s[0] === 'return') break; // `return playerbot_equipment;`
			$I->exec($s, $frames, $top);
		}
		$args = [
			['table'], ['table'], ['num', $class], ['num', 1],
			['bool', $size_name === 'small'], ['bool', $size_name === 'medium'], ['bool', $size_name === 'large'],
			['level'],
		];
		$I->call_func('playerbot_equipment.GenerateLoot', $args, $top, 0);

		// Order is by execution position; two draws on one position in one run
		// would make it ambiguous.
		$seen = [];
		foreach ($I->draws as $d) {
			$pk = implode('.', $d['pos']);
			if (isset($seen[$pk])) fail("two draws at position $pk (class $class, size $size_name)");
			$seen[$pk] = true;
		}
		$runs[] = ['class' => $class, 'size' => $size_bit, 'interp' => $I];
	}
}

// ---------------------------------------------------------------------------
// Merge identical draws across runs
// ---------------------------------------------------------------------------
//
// Identity: execution position + level range + chance + pool + (merged) gate.
// Processed in position order so a gate's merged id exists before any draw
// that references it. Lexicographic order of the call-stack position IS the
// Lua execution order.

$all = [];
foreach ($runs as $ri => $run) {
	foreach ($run['interp']->draws as $di => $d) {
		$d['run'] = $ri;
		$d['poskey'] = implode('.', array_map(function ($x) { return sprintf('%05d', $x); }, $d['pos']));
		$all[] = $d;
	}
}
usort($all, function ($a, $b) {
	$c = strcmp($a['poskey'], $b['poskey']);
	if ($c) return $c;
	if ($a['lo'] !== $b['lo']) return $a['lo'] <=> $b['lo'];
	return $a['run'] <=> $b['run'];
});

$merged = [];        // id => row
$by_key = [];        // identity => id
$flag_merged = [];   // "run:ref" => merged id
$next_id = 1;

foreach ($all as $d) {
	$gate_id = null; $gate_hit = 1;
	if ($d['gate'] !== null) {
		$k = $d['run'] . ':' . $d['gate'][0];
		if (!isset($flag_merged[$k])) fail("gate references a flag that was never merged");
		$gate_id = $flag_merged[$k];
		$gate_hit = $d['gate'][1] ? 1 : 0;
	}
	ksort($d['pool']);
	$pool_key = json_encode($d['pool']);
	$key = implode('|', [$d['kind'], $d['poskey'], $d['lo'], $d['hi'], $d['chance'], $pool_key, $gate_id ?? '-', $gate_hit]);

	$run = $runs[$d['run']];
	if (!isset($by_key[$key])) {
		$id = $next_id++;
		$by_key[$key] = $id;
		$merged[$id] = [
			'id' => $id, 'kind' => $d['kind'], 'pos' => $d['pos'],
			'lo' => $d['lo'], 'hi' => $d['hi'], 'chance' => $d['chance'],
			'gate_id' => $gate_id, 'gate_hit' => $gate_hit,
			'classes' => 0, 'sizes' => 0, 'pool' => $d['pool'],
		];
		if ($d['kind'] === 'item' && !$d['pool']) $empty_item_draws++;
	}
	$id = $by_key[$key];
	$merged[$id]['classes'] |= 1 << ($run['class'] - 1);
	$merged[$id]['sizes']   |= $run['size'];
	if ($d['kind'] === 'flag') $flag_merged[$d['run'] . ':' . $d['ref']] = $id;
}

// An item draw whose pool is empty adds nothing; keep it out of the table
// unless something gates on it (item draws are never gates).
$merged = array_filter($merged, function ($m) { return !($m['kind'] === 'item' && !$m['pool']); });

// Drop flag draws nothing references (e.g. `dual_wield`, assigned but unused).
$referenced = [];
foreach ($merged as $m) if ($m['gate_id'] !== null) $referenced[$m['gate_id']] = true;
$changed = true;
while ($changed) {
	$changed = false;
	foreach ($merged as $id => $m) {
		if ($m['kind'] === 'flag' && !isset($referenced[$id])) {
			unset($merged[$id]);
			$changed = true;
			$referenced = [];
			foreach ($merged as $mm) if ($mm['gate_id'] !== null) $referenced[$mm['gate_id']] = true;
			break;
		}
	}
}

// Integer item weights per draw: scale the exact rationals by the LCM.
function lcm($a, $b) { return intdiv($a * $b, gcd($a, $b)); }
foreach ($merged as &$m) {
	$l = 1;
	foreach ($m['pool'] as $w) $l = lcm($l, $w[1]);
	$ints = [];
	foreach ($m['pool'] as $id => $w) $ints[$id] = intdiv($w[0] * $l, $w[1]);
	$g = 0;
	foreach ($ints as $v) $g = gcd($g, $v);
	foreach ($ints as $id => $v) $ints[$id] = intdiv($v, $g ?: 1);
	$m['weights'] = $ints;
}
unset($m);

// Renumber densely in order so ids read as "draw order".
$final = []; $remap = []; $n = 1;
foreach ($merged as $id => $m) { $remap[$id] = $n++; }
foreach ($merged as $id => $m) {
	$m['id'] = $remap[$id];
	if ($m['gate_id'] !== null) $m['gate_id'] = $remap[$m['gate_id']];
	$final[] = $m;
}

// ---------------------------------------------------------------------------
// Emit
// ---------------------------------------------------------------------------

$item_rows = 0;
foreach ($final as $m) $item_rows += count($m['weights']);

$sql = [];
$sql[] = "-- ===========================================================================";
$sql[] = "-- PlayerBot native gear -- GENERATED by utils/scripts/playerbot_native/lua_gear_to_sql.php";
$sql[] = "-- from lua_modules/playerbot_equipment.lua. Do not hand-edit the bulk of this";
$sql[] = "-- file: change the Lua (or the rows in the database) and regenerate.";
$sql[] = "--";
$sql[] = "-- " . count($final) . " draws, $item_rows item rows. Run AFTER 2026_10_18_bots_playerbot_native.sql.";
$sql[] = "-- Re-runnable: it empties both gear tables first (they hold no player state).";
$sql[] = "--";
$sql[] = "-- Validation: utils/scripts/playerbot_native/validate_gear.lua runs the ORIGINAL";
$sql[] = "-- Lua and compares its item distributions against these rows (see README.md).";
$sql[] = "-- ===========================================================================";
$sql[] = "";
$sql[] = "SELECT COUNT(*) AS gear_draws_before FROM playerbot_gear_draws;";
$sql[] = "SELECT COUNT(*) AS gear_items_before FROM playerbot_gear_items;";
$sql[] = "";
$sql[] = "DELETE FROM playerbot_gear_items;";
$sql[] = "DELETE FROM playerbot_gear_draws;";
$sql[] = "";

$chunks = array_chunk($final, 200);
foreach ($chunks as $chunk) {
	$vals = [];
	foreach ($chunk as $m) {
		$note = ($m['kind'] === 'flag' ? 'flag' : 'item') . ' @' . implode('>', $m['pos']);
		$vals[] = sprintf("(%d, %d, %d, %d, %d, %d, %s, %d, '%s')",
			$m['id'], $m['lo'], $m['hi'], $m['classes'], $m['sizes'], $m['chance'],
			$m['gate_id'] === null ? 'NULL' : $m['gate_id'], $m['gate_hit'], $note);
	}
	$sql[] = "INSERT INTO playerbot_gear_draws (id, min_level, max_level, classes, sizes, chance, gate_id, gate_hit, note) VALUES";
	$sql[] = implode(",\n", $vals) . ";";
	$sql[] = "";
}

$item_vals = [];
foreach ($final as $m) foreach ($m['weights'] as $item => $w) $item_vals[] = sprintf("(%d, %d, %d)", $m['id'], $item, $w);
foreach (array_chunk($item_vals, 500) as $chunk) {
	$sql[] = "INSERT INTO playerbot_gear_items (draw_id, item_id, weight) VALUES";
	$sql[] = implode(",\n", $chunk) . ";";
	$sql[] = "";
}

$sql[] = "SELECT COUNT(*) AS gear_draws_after FROM playerbot_gear_draws;";
$sql[] = "SELECT COUNT(*) AS gear_items_after FROM playerbot_gear_items;";
$sql[] = "-- Item ids the gear names but the items table lacks (the engine skips them):";
$sql[] = "SELECT DISTINCT gi.item_id FROM playerbot_gear_items gi LEFT JOIN items i ON i.id = gi.item_id WHERE i.id IS NULL;";

file_put_contents($out_sql, implode("\n", $sql) . "\n");

// Lua table for validate_gear.lua
$lua = ["-- GENERATED by lua_gear_to_sql.php -- the draw rows, for validate_gear.lua", "return {"];
foreach ($final as $m) {
	$items = [];
	foreach ($m['weights'] as $item => $w) $items[] = "{{$item},{$w}}";
	$lua[] = sprintf("{id=%d,lo=%d,hi=%d,classes=%d,sizes=%d,chance=%d,gate=%s,gate_hit=%s,items={%s}},",
		$m['id'], $m['lo'], $m['hi'], $m['classes'], $m['sizes'], $m['chance'],
		$m['gate_id'] === null ? 'nil' : $m['gate_id'], $m['gate_hit'] ? 'true' : 'false', implode(',', $items));
}
$lua[] = "}";
file_put_contents($out_lua, implode("\n", $lua) . "\n");

fwrite(STDOUT, sprintf("ok: %d runs, %d draws (%d flags), %d item rows, %d empty item draws dropped\n",
	count($runs), count($final), count(array_filter($final, function ($m) { return $m['kind'] === 'flag'; })),
	$item_rows, $empty_item_draws));
