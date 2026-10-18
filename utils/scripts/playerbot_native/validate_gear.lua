--[[
validate_gear.lua -- checks the generated gear rows against the ORIGINAL Lua.

Run by run_validate_gear.ps1 (hosts lua51.dll, the interpreter zone.exe uses).
Globals set by the host before this runs:
  EQUIPMENT_LUA  path to lua_modules/playerbot_equipment.lua
  DRAWS_LUA      path to the draws table emitted by lua_gear_to_sql.php
  REPORT_PATH    where to write the report
  SAMPLES        Monte Carlo runs per (class, size, level)

For every class x race size x probe level it:
  1. runs the original GenerateLoot SAMPLES times, with eq.ChooseRandom made
     explicit about nil: drop nil arguments, return nil when nothing is left.
     Under LuaJIT that is already what happens ({a, b, nil} has length 2 and
     math.random(0) does not raise), so this pins the behaviour rather than
     changing it.
  2. computes, EXACTLY, the expected count of every item the draw rows give
     a bot of that class/size/level (the playerbot_native.cpp algorithm).
  3. fails any item whose Monte Carlo mean is outside the tolerance, or that
     one side produces and the other never does.

It also runs the UNPATCHED original once per configuration and counts any that
abort (a direct AddItem of an empty picker would).
]]

local report = {}
local function out(fmt, ...) report[#report + 1] = string.format(fmt, ...) end

local draws = dofile(DRAWS_LUA)
local by_id = {}
for i, d in ipairs(draws) do by_id[d.id] = i end

-- -------------------------------------------------------------------------
-- The original module, with a recording NPC and a switchable ChooseRandom
-- -------------------------------------------------------------------------

local patched = true
local function choose_random(...)
	local n = select('#', ...)
	if patched then
		local t = {}
		for i = 1, n do
			local v = select(i, ...)
			if v ~= nil then t[#t + 1] = v end
		end
		if #t == 0 then return nil end
		return t[math.random(#t)]
	end
	local t = {...}
	return t[math.random(#t)] -- math.random(0) raises: the original behaviour
end

local eq = { ChooseRandom = choose_random }
local added
local e = { self = { AddItem = function(_, id, charges, equip)
	-- the real Lua_NPC::AddItem is a luabind overload set: a nil id has no
	-- matching overload and raises, aborting the rest of event_spawn
	if id == nil and not patched then error("AddItem(nil)") end
	if id ~= nil then added[#added + 1] = id end
end } }

local equipment = dofile(EQUIPMENT_LUA)

local sizes = { [1] = "small", [2] = "medium", [4] = "large" }
local levels = { 1, 5, 9, 10, 11, 12, 13, 15, 16, 17, 18, 20, 21, 25, 30, 31, 35, 40, 41, 45, 50, 55, 60 }

local function lua_expectation(class, size_bit, level, samples)
	local counts = {}
	local small, medium, large = size_bit == 1, size_bit == 2, size_bit == 4
	for _ = 1, samples do
		added = {}
		equipment.GenerateLoot(e, eq, class, 1, small, medium, large, level)
		for _, id in ipairs(added) do counts[id] = (counts[id] or 0) + 1 end
	end
	for id, c in pairs(counts) do counts[id] = c / samples end
	return counts
end

local function lua_aborts(class, size_bit, level)
	patched = false
	added = {}
	local ok = pcall(equipment.GenerateLoot, e, eq, class, 1, size_bit == 1, size_bit == 2, size_bit == 4, level)
	patched = true
	return not ok
end

-- -------------------------------------------------------------------------
-- Exact expectation from the draw rows (playerbot_native.cpp ApplyGear)
-- -------------------------------------------------------------------------

local function native_expectation(class, size_bit, level)
	local class_bit = 2 ^ (class - 1)
	local p_hit, p_miss = {}, {}
	local exp = {}
	for i, d in ipairs(draws) do
		local eligible = 0
		local cls_ok = math.floor(d.classes / class_bit) % 2 == 1
		local size_ok = math.floor(d.sizes / size_bit) % 2 == 1
		if level >= d.lo and level <= d.hi and cls_ok and size_ok then
			eligible = 1
			if d.gate then
				local g = by_id[d.gate]
				eligible = d.gate_hit and p_hit[g] or p_miss[g]
			end
		end
		local c = d.chance / 100
		p_hit[i] = eligible * c
		p_miss[i] = eligible * (1 - c)
		if p_hit[i] > 0 and #d.items > 0 then
			local total = 0
			for _, it in ipairs(d.items) do total = total + it[2] end
			for _, it in ipairs(d.items) do
				exp[it[1]] = (exp[it[1]] or 0) + p_hit[i] * it[2] / total
			end
		end
	end
	return exp
end

-- -------------------------------------------------------------------------

math.randomseed(20261018)

local configs, failures, aborting = 0, 0, 0
local worst = 0

for class = 1, 14 do
	for size_bit, size_name in pairs(sizes) do
		for _, level in ipairs(levels) do
			configs = configs + 1
			if lua_aborts(class, size_bit, level) then aborting = aborting + 1 end

			local mc = lua_expectation(class, size_bit, level, SAMPLES)
			local ex = native_expectation(class, size_bit, level)

			local ids = {}
			for id in pairs(mc) do ids[id] = true end
			for id in pairs(ex) do ids[id] = true end

			for id in pairs(ids) do
				local a, b = mc[id] or 0, ex[id] or 0
				-- a per-bot count is a sum of Bernoullis; var <= mean
				local tol = 5 * math.sqrt(math.max(a, b) / SAMPLES) + 0.002
				local bad = math.abs(a - b) > tol
				if (b == 0 and a > 0) then bad = true end           -- Lua gives it, rows never do
				if (a == 0 and b > 25 / SAMPLES) then bad = true end -- rows give it often, Lua never did
				if bad then
					failures = failures + 1
					if failures <= 60 then
						out("MISMATCH class %d size %s level %d item %d: lua %.4f rows %.4f", class, size_name, level, id, a, b)
					end
				end
				local d = math.abs(a - b) / (tol > 0 and tol or 1)
				if d > worst then worst = d end
			end
		end
	end
end

out("configs %d, samples each %d", configs, SAMPLES)
-- Under LuaJIT (what zone.exe runs) math.random(0) does not raise, so an empty
-- eq.ChooseRandom returns nil; nested inside another pool it simply vanishes.
-- Only a direct AddItem(<empty picker>) would abort, and this counts those.
out("unpatched original aborts GenerateLoot in %d of %d configurations", aborting, configs)
out("mismatches: %d (worst deviation %.2f of tolerance)", failures, worst)
out(failures == 0 and "RESULT: PASS" or "RESULT: FAIL")

local f = io.open(REPORT_PATH, "w")
f:write(table.concat(report, "\n"), "\n")
f:close()
return failures == 0 and "PASS" or "FAIL"
