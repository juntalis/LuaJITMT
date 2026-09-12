local ffi = require("ffi")
local threading = require("threading")
local exdata = require("thread.exdata")

assert(rawequal(exdata, threading.exdata))
local null = exdata()
assert(ffi.istype("void *", null) and null == nil)

local a = ffi.cast("void *", 0x1234)
local b = ffi.cast("int *", 0x5678)
exdata(a)
assert(exdata() == a)
exdata(b)
assert(exdata() == ffi.cast("void *", b))
assert(not pcall(exdata, 1))
assert(not pcall(exdata, "pointer"))
assert(not pcall(exdata, 1LL))
assert(not pcall(exdata, a, b))

exdata(a)
local co = coroutine.create(function()
  assert(exdata() == a)
  exdata(b)
  coroutine.yield(true)
  assert(exdata() == ffi.cast("void *", b))
  return true
end)
exdata(nil)
assert(coroutine.resume(co))
assert(exdata() == nil)
assert(coroutine.resume(co))
assert(exdata() == nil)

exdata(a)
local worker = threading.spawn(function(expected)
  local ffi = require("ffi")
  local getset = require("thread.exdata")
  local inherited = tonumber(ffi.cast("uintptr_t", getset()))
  getset(ffi.cast("void *", 0x9abc))
  return inherited == expected and
         tonumber(ffi.cast("uintptr_t", getset())) == 0x9abc
end, 0x1234)
local joined, result = worker:join(10)
assert(joined == true and result == true)
assert(exdata() == a)

jit.opt.start("hotloop=1")
local function hot_get(expected)
  local matched = 0
  for _ = 1, 200 do
    if exdata() == expected then matched = matched + 1 end
  end
  return matched
end
assert(hot_get(a) == 200)

local function hot_set(value)
  for _ = 1, 100 do exdata(value) end
end
hot_set(b)
assert(exdata() == ffi.cast("void *", b))
exdata(nil)

collectgarbage()
assert(exdata() == nil)
print("compat exdata Lua surface passed")
