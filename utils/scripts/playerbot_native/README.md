# playerbot_native converters

These scripts turn the Lua PlayerBot content into the `playerbot_*` tables that
`zone/playerbot_native.cpp` reads. Re-run them whenever the Lua changes, while
the Lua is still the source.

You need XAMPP's `php.exe` (x64, 7.4+). The validator also needs the server's
`lua51.dll`. Nothing here builds the server.

| Script | Reads | Writes |
|---|---|---|
| `lua_gear_to_sql.php` | `lua_modules/playerbot_equipment.lua` | `utils/sql/git/bots/optional/2026_10_18_bots_playerbot_native_gear.sql` and a draws table for the validator |
| `lua_trades_to_sql.php` | `lua_modules/playerbot_trading.lua` | `playerbot_trades` rows, spliced into `2026_10_18_bots_playerbot_native.sql` |
| `validate_gear.lua` + `run_lua.php` | the original equipment Lua and the draws table | a PASS/FAIL report |

## Regenerating the gear

```sh
php lua_gear_to_sql.php C:/eqemu/lua_modules/playerbot_equipment.lua \
    ../../sql/git/bots/optional/2026_10_18_bots_playerbot_native_gear.sql \
    <scratch>/gear_draws.lua
```

The equipment Lua is a decision tree, so the script interprets it rather than
grepping it. For example, it rolls "one-handed?" and only rolls a shield if
that hit. The script runs once per class × race size, keeping level as a
symbolic interval and turning every boolean `eq.ChooseRandom` into a gate
draw. It stops with an error on any Lua construct outside the subset it
understands.

## Validating

```sh
php -d extension=ffi -d ffi.enable=1 run_lua.php C:/eqemu/lua51.dll validate_gear.lua \
    EQUIPMENT_LUA=C:/eqemu/lua_modules/playerbot_equipment.lua \
    DRAWS_LUA=<scratch>/gear_draws.lua REPORT_PATH=<scratch>/report.txt SAMPLES=6000
```

The validator compares the two implementations for 14 classes × 3 sizes × 23
probe levels:

- it runs the **original** `GenerateLoot` through LuaJIT, the interpreter
  zone.exe embeds, and measures item frequencies by Monte Carlo;
- it computes the **exact** expected item counts that the generated rows give.

Run on 2026-10-18 at 6000 samples, it reported 0 mismatches, with the worst
deviation at 0.73 of tolerance. Removing every gate from the rows makes it
fail with 5599 mismatches, so it can catch a broken conversion.

The PHP host is used because PowerShell 5.1 is native ARM64 on some machines
and cannot load the x64 `lua51.dll`.
