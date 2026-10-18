<?php
/*
 * run_lua.php -- hosts lua51.dll (the LuaJIT build zone.exe ships with) through
 * PHP FFI, so the validators can run the ORIGINAL Lua modules without a
 * standalone lua.exe. PHP is used rather than PowerShell because PowerShell is
 * native ARM64 on some machines and cannot load an x64 DLL; XAMPP's php.exe is x64.
 *
 *   php -d extension=ffi -d ffi.enable=1 run_lua.php <lua51.dll> <script.lua> [NAME=value ...]
 *
 * Each NAME=value becomes a Lua global (a number when numeric, else a string)
 * before <script.lua> runs. The script's return value is printed; a Lua error
 * prints "LUA ERROR: ..." and exits 1.
 */

if ($argc < 3) {
	fwrite(STDERR, "usage: php -d extension=ffi -d ffi.enable=1 run_lua.php <lua51.dll> <script.lua> [NAME=value ...]\n");
	exit(2);
}

$lua = FFI::cdef('
	typedef struct lua_State lua_State;
	lua_State *luaL_newstate(void);
	void luaL_openlibs(lua_State *L);
	int luaL_loadstring(lua_State *L, const char *s);
	int lua_pcall(lua_State *L, int nargs, int nresults, int errfunc);
	const char *lua_tolstring(lua_State *L, int idx, size_t *len);
	void lua_close(lua_State *L);
', $argv[1]);

function lua_quote($s) { return '[==[' . str_replace('\\', '/', $s) . ']==]'; }

$chunk = '';
for ($i = 3; $i < $argc; ++$i) {
	[$k, $v] = explode('=', $argv[$i], 2);
	$chunk .= $k . ' = ' . (is_numeric($v) ? $v : lua_quote($v)) . "\n";
}
$chunk .= 'return dofile(' . lua_quote($argv[2]) . ")\n";

$L = $lua->luaL_newstate();
$lua->luaL_openlibs($L);
$rc = $lua->luaL_loadstring($L, $chunk);
if ($rc === 0) {
	$rc = $lua->lua_pcall($L, 0, 1, 0);
}
// FFI already converts a returned const char* to a PHP string
$p = $lua->lua_tolstring($L, -1, null);
$result = $p === null ? '(no result)' : (string)$p;
$lua->lua_close($L);

if ($rc !== 0) {
	echo "LUA ERROR: $result\n";
	exit(1);
}
echo "$result\n";
exit($result === 'FAIL' ? 1 : 0);
