local dump = require "dump"

local failed = 0
local function check(cond, msg)
	if not cond then
		failed = failed + 1
		io.stderr:write("FAIL: ", msg or "check failed", "\n")
	end
end

local function check_err(f, needle)
	local ok, err = pcall(f)
	check(not ok, "expected error")
	check(type(err) == "string" and err:find(needle, 1, true), "error message: " .. tostring(err))
end

local function eq(a, b, msg)
	if a ~= b then
		check(false, msg or "eq")
		io.stderr:write("  got:  ", tostring(a):gsub("\n", "\\n"), "\n")
		io.stderr:write("  want: ", tostring(b):gsub("\n", "\\n"), "\n")
	end
end

-- scalars / multi-arg
eq(dump.packstring(), "", "empty")
eq(dump.packstring(nil), "nil", "nil")
eq(dump.packstring(true), "true", "true")
eq(dump.packstring(false), "false", "false")
eq(dump.packstring(42), "42", "int")
eq(dump.packstring(-1), "-1", "neg")
eq(dump.packstring(1, 2, 3), "1 2 3", "multi")
eq(dump.packstring("a\nb"), '"a\\nb"', "nl")
eq(dump.packstring("a\tb\r\"\\"), '"a\\tb\\r\\"\\\\"', "esc")
eq(dump.packstring(string.char(0, 1, 127)), '"\\x00\\x01\\x7f"', "ctrl")

-- empty / nested table
eq(dump.packstring({}), "{\n\n}", "empty table")
local t = { 1, 2, a = { b = 3 } }
local s = dump.packstring(t)
check(s:find("1", 1, true), "array 1")
check(s:find("2", 1, true), "array 2")
check(s:find("a =", 1, true), "hash key")
check(s:find("b = 3", 1, true), "nested")

-- ident vs quoted key
check(dump.packstring({ ["foo_1"] = 1 }):find("foo_1 = 1", 1, true), "ident key")
check(dump.packstring({ ["a-b"] = 1 }):find('"a-b" = 1', 1, true), "quoted key")
check(dump.packstring({ [""] = 1 }):find('"" = 1', 1, true), "empty string key")
check(dump.packstring({ [true] = 1 }):find("[true] = 1", 1, true), "bool key")

-- cycle
local c = { x = 1 }
c.self = c
check(dump.packstring(c):find("<cycle>", 1, true), "cycle marker")

-- table as key
local k = { 1 }
local tk = {}
tk[k] = 2
check(dump.packstring(tk):find("2", 1, true), "table as key")

-- unsupported types
check(dump.packstring(function() end):find("<function>", 1, true), "function")
check(dump.packstring(coroutine.create(function() end)):find("<thread>", 1, true), "thread")

-- depth truncate (MAX_DEPTH=32；须在 C 栈溢出前截断)
do
	local deep = {}
	local cur = deep
	for _ = 1, 40 do
		cur.n = {}
		cur = cur.n
	end
	local ds = dump.packstring(deep)
	check(ds:find("<...>", 1, true), "depth truncate")
	-- 刚好 32 层内不应截断
	deep, cur = {}, nil
	cur = deep
	for _ = 1, 16 do
		cur.n = {}
		cur = cur.n
	end
	check(not dump.packstring(deep):find("<...>", 1, true), "depth 16 no truncate")
end

-- array hole
do
	local a = { 1, nil, 3 }
	a[3] = 3
	check(dump.packstring(a):find("1", 1, true), "array hole has 1")
end

-- __pairs
do
	local t = { hidden = 1 }
	setmetatable(t, {
		__pairs = function()
			return next, { visible = 2, ["x-y"] = 3 }, nil
		end,
	})
	local ps = dump.packstring(t)
	check(ps:find("visible = 2", 1, true), "__pairs visible")
	check(ps:find('"x-y" = 3', 1, true), "__pairs quoted key")
	check(not ps:find("hidden", 1, true), "__pairs hides raw")
end

-- __pairs + cycle
do
	local data = { a = 1 }
	data.self = data
	local proxy = setmetatable({}, {
		__pairs = function()
			return next, data, nil
		end,
	})
	local ps = dump.packstring(proxy)
	check(ps:find("a = 1", 1, true), "__pairs nested data")
	check(ps:find("<cycle>", 1, true), "__pairs cycle")
end

-- metamethod 抛错须原样传播（缓冲由 __gc 兜底回收）
do
	local boom = setmetatable({ 1, 2, x = "y" }, {
		__pairs = function()
			error("pairs-boom", 0)
		end,
	})
	check_err(function() dump.packstring(boom) end, "pairs-boom")
	check_err(function() dump.pack(boom) end, "pairs-boom")
end

-- pack / trash
do
	local ptr, sz = dump.pack()
	check(type(ptr) == "userdata", "empty pack ptr type")
	eq(sz, 0, "empty pack sz")
	dump.trash(ptr, sz)

	ptr, sz = dump.pack(1, 2)
	check(type(ptr) == "userdata", "pack lightuserdata")
	eq(sz, 3, "pack sz for '1 2'")
	eq(dump.packstring(1, 2), "1 2", "packstring 1 2")
	dump.trash(ptr, sz)

	local sample = { 1, 2, a = "x", b = { c = true } }
	local ss = dump.packstring(sample)
	ptr, sz = dump.pack(sample)
	eq(sz, #ss, "pack sz == packstring len")
	dump.trash(ptr, sz)
end

dump.trash("ignore", 0)
check_err(function() dump.trash(1, 0) end, "invalid param")
check_err(function() dump.trash(nil, 0) end, "invalid param")

check(dump.packstring(1.5):find("1.5", 1, true), "float")

if failed > 0 then
	io.stderr:write(string.format("%d check(s) failed\n", failed))
	os.exit(1)
end
print("ok")
