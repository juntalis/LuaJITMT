local build = require("suite_build")
local runtime = require("suite_runtime")
local utils = require("suite_utils")

local cases = {}

local function compile_fixture(t, name, source)
  local out = t:tmp(name)
  t:cc(out, { t:path("tests", "fixtures", source) }, {
    cflags = "-std=gnu11 -O2 -Wall -Wextra -Werror",
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
  return out
end

local function run_api52(t)
  build.with_default_build_restore(t, function()
    t:build({ clean = true, quiet = true })
    local source = t:path("tests", "fixtures", "compat-api52.c")
    local static = compile_fixture(t, "compat-api52-static", "compat-api52.c")
    t:run({ static, "0" }, { quiet = true, timeout = "30s" })

    local shared = t:tmp("compat-api52-shared")
    t:cc(shared, { source }, {
      cflags = "-std=gnu11 -O2 -Wall -Wextra -Werror",
      link_luajit = false,
      libs = { "-L" .. t:path("src"), "-lluajit", "-lm", "-ldl", "-pthread" }
    })
    t:run({ shared, "0" }, {
      quiet = true,
      timeout = "30s",
      env = {
        LD_LIBRARY_PATH = t:path("src"),
        LD_PRELOAD = t:path("src", "libluajit.so")
      }
    })

    local exports = utils.capture_command(
      "nm -D " .. utils.shell_quote(t:path("src", "libluajit.so")))
    for _, symbol in ipairs({
      "lua_absindex", "lua_rawlen", "lua_len", "lua_setexdata",
      "lua_getexdata", "luaL_len", "luaL_getsubtable", "luaL_tolstring",
      "luaL_pushresultsize", "luaL_requiref", "luaJIT_compat52",
      "luaopen_utf8"
    }) do
      assert(exports:find(symbol, 1, true), "shared export missing: " .. symbol)
    end

    t:build({ clean = true, quiet = true,
              xcflags = "-DLUAJIT_ENABLE_LUA52COMPAT" })
    local compat = compile_fixture(t, "compat-api52-lj52", "compat-api52.c")
    t:run({ compat, "1" }, { quiet = true, timeout = "30s" })
  end)
  print("compat API 5.2 passed")
end

local function run_bcdump(t)
  t:build({ quiet = true })
  local source = t:path("tests", "fixtures", "compat-bcdump.lua")
  local basic = t:tmp("compat-bcdump-basic.txt")
  local lines = t:tmp("compat-bcdump-lines.txt")
  local constants = t:tmp("compat-bcdump-constants.txt")
  local combined1 = t:tmp("compat-bcdump-combined-1.txt")
  local combined2 = t:tmp("compat-bcdump-combined-2.txt")
  local raw = t:tmp("compat-bcdump-stripped.raw")
  local stripped = t:tmp("compat-bcdump-stripped.txt")
  runtime.capture_luajit(t, { "-bl", source, "-" }, basic, { quiet = true })
  runtime.capture_luajit(t, { "-bL", source, "-" }, lines, { quiet = true })
  runtime.capture_luajit(t, { "-bk", source, "-" }, constants, { quiet = true })
  runtime.capture_luajit(t, { "-bLk", source, "-" }, combined1, { quiet = true })
  runtime.capture_luajit(t, { "-bLk", source, "-" }, combined2, { quiet = true })
  runtime.luajit(t, { "-bs", source, raw }, { quiet = true })
  runtime.capture_luajit(t, { "-bL", raw, "-" }, stripped, { quiet = true })

  local b = utils.read_file(basic)
  local l = utils.read_file(lines)
  local k = utils.read_file(constants)
  local both1 = utils.read_file(combined1)
  local both2 = utils.read_file(combined2)
  assert(not b:find("KGC ", 1, true) and not b:find("KN ", 1, true))
  assert(not b:find("%[%d+%]"))
  assert(l:find("%[%d+%]") and not l:find("KGC ", 1, true))
  assert(k:find("KGC 0 ", 1, true) and k:find("KN 0 123.25", 1, true))
  assert(k:find('"line\\nvalue\\000"', 1, true))
  assert(both1 == both2 and both1:find("%[%d+%]") and
         both1:find("KGC 0 ", 1, true))
  assert(utils.read_file(stripped):find("%[%-%]"))

  runtime.luajit_code(t, [[
    local util = require("jit.util")
    local bc = require("jit.bc")
    local function f() return 1 end
    local ins, mode = util.funcbc(f, 1)
    assert(ins and mode)
    local ins2, mode2, line = util.funcbc(f, 1, true)
    assert(ins2 == ins and mode2 == mode and line > 0)
    local stripped = assert(loadstring(string.dump(f, true)))
    local _, _, no_line = util.funcbc(stripped, 1, true)
    assert(no_line == 0)
    local out = { text = "" }
    function out:write(s) self.text = self.text .. s end
    function out:flush() end
    bc.dump(f, out, false, true)
    assert(out.text:find("%[%d+%]"))
  ]], { quiet = true })

  local dump_b = t:tmp("compat-jit-dump-b.txt")
  local dump_bl = t:tmp("compat-jit-dump-bl.txt")
  local dump_l = t:tmp("compat-jit-dump-l.txt")
  local dump_bhl = t:tmp("compat-jit-dump-bhl.html")
  local code = string.format([[
    local dump = require("jit.dump")
    jit.opt.start("hotloop=1")
    dump.on("b", %q)
    local x = 0; for i=1,100 do x=x+i end
    dump.off(); jit.flush()
    dump.on("bL", %q)
    local y = 0; for i=1,100 do y=y+i end
    dump.off(); jit.flush()
    dump.on("L", %q)
    local z = 0; for i=1,100 do z=z+i end
    dump.off(); jit.flush()
    local html_loop = assert(loadstring(
      "return function() local n=0 for i=1,100 do n=n+i end return n end",
      "@a<b&c>.lua"))()
    dump.on("bHL", %q)
    local h = html_loop()
    dump.off()
    assert(x == 5050 and y == 5050 and z == 5050 and h == 5050)
  ]], dump_b, dump_bl, dump_l, dump_bhl)
  runtime.luajit_code(t, code, { quiet = true })
  local plain = utils.read_file(dump_b)
  local located = utils.read_file(dump_bl)
  local location_only = utils.read_file(dump_l)
  local html = utils.read_file(dump_bhl)
  assert(not plain:find("%([^\n]-:%d+%)"))
  assert(located:find("%([^\n]-:%d+%)"))
  assert(not location_only:find("BYTECODE", 1, true))
  assert(html:find("a&lt;b&amp;c&gt;.lua", 1, true))
  print("compat bytecode inspection passed")
end

local function run_os_date_tz(t)
  t:build({ quiet = true })
  local out = compile_fixture(t, "compat-os-date-tz", "compat-exdata-tz.c")
  t:run({ out, "tz" }, { quiet = true, timeout = "30s" })
  print("compat os.date timezone refresh passed")
end

local function run_pattern_nul(t)
  t:build({ quiet = true })
  local script = t:path("tests", "fixtures", "compat-pattern-nul.lua")
  runtime.luajit(t, { "-joff", script }, { quiet = true })
  runtime.luajit(t, { "-jon", script }, { quiet = true })
  print("compat binary-safe patterns passed")
end

local function run_exdata(t)
  build.with_default_build_restore(t, function()
    t:build({ clean = true, quiet = true })
    local out = compile_fixture(t, "compat-exdata", "compat-exdata-tz.c")
    t:run({ out, "exdata" }, { quiet = true, timeout = "30s" })
    local script = t:path("tests", "fixtures", "compat-exdata.lua")
    runtime.luajit(t, { "-joff", script }, { quiet = true,
                                              timeout = "30s" })
    runtime.luajit(t, { "-jon", script }, { quiet = true,
                                             timeout = "30s" })

    t:build({ clean = true, quiet = true,
              xcflags = "-DLUAJIT_DISABLE_FFI" })
    local basic = compile_fixture(t, "compat-exdata-noffi", "compat-exdata-tz.c")
    t:run({ basic, "exdata-basic" }, { quiet = true, timeout = "30s" })
    runtime.luajit_code(t, [[
      assert(require("threading").exdata == nil)
      assert(package.preload["thread.exdata"] == nil)
    ]], { quiet = true })
  end)
  print("compat exdata passed")
end

local function run_stdlib53(t)
  t:build({ quiet = true })
  local script = t:path("tests", "fixtures", "compat-stdlib53.lua")
  runtime.luajit(t, { "-joff", script }, { quiet = true })
  runtime.luajit(t, { "-jon", script }, { quiet = true })
  print("compat Lua 5.3 standard libraries passed")
end

cases.compat_api52 = run_api52
cases.compat_bcdump = run_bcdump
cases.compat_os_date_tz = run_os_date_tz
cases.compat_pattern_nul = run_pattern_nul
cases.compat_exdata = run_exdata
cases.compat_stdlib53 = run_stdlib53

return function(add)
  local order = {
    "compat_api52", "compat_bcdump", "compat_os_date_tz",
    "compat_pattern_nul", "compat_exdata", "compat_stdlib53"
  }
  for _, name in ipairs(order) do
    add({ name = name, description = "MoonJIT compatibility extraction: " .. name,
          run = cases[name] })
  end
  add({
    name = "compat_extensions",
    description = "all selected MoonJIT compatibility extensions",
    deps = order,
    run = function(t)
      for _, name in ipairs(order) do
        io.stderr:write("== " .. name .. " ==\n")
        cases[name](t)
      end
      print("compat extensions aggregate passed")
    end
  })
end
