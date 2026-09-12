local function fails(fn, ...)
  return not pcall(fn, ...)
end

assert(math.maxinteger == 2^53 and math.mininteger == -2^53)
for _, v in ipairs({ -2^53, -1, -0.0, 0, 1, 2^53 }) do
  assert(math.tointeger(v) == v)
  assert(math.type(v) == "integer")
end
for _, v in ipairs({ -2^53 - 2, 2^53 + 2, 0/0, 1/0, -1/0, 1.5 }) do
  assert(math.tointeger(v) == nil)
  assert(math.type(v) == "float")
end
assert(math.tointeger("1") == nil and math.type("1") == nil)
assert(math.tointeger(true) == nil and math.type(true) == nil)
assert(1 / math.tointeger(-0.0) == -1/0)
assert(math.ult(0, 1) and math.ult(0, -1) and not math.ult(-1, 0))
assert(math.ult(-2, -1) and not math.ult(-1, -2))
local ok, err = pcall(math.ult, 1.5, 0)
assert(not ok and tostring(err):find("bad argument #1", 1, true))
ok, err = pcall(math.ult, 0, 1.5)
assert(not ok and tostring(err):find("bad argument #2", 1, true))
assert(fails(math.ult, "1", 0) and fails(math.ult, 0, "1"))

for size = 1, 16 do
  local sfmt, ufmt = "<i" .. size, ">I" .. size
  local packed = string.pack(sfmt, -1)
  local value, pos = string.unpack(sfmt, packed)
  assert(value == -1 and pos == #packed + 1 and #packed == size)
  packed = string.pack(ufmt, 1)
  value, pos = string.unpack(ufmt, packed)
  assert(value == 1 and pos == #packed + 1 and #packed == size)
end
for size = 1, 6 do
  local bits = size * 8
  local smin, smax, umax = -2^(bits - 1), 2^(bits - 1) - 1, 2^bits - 1
  assert(string.unpack("i" .. size, string.pack("i" .. size, smin)) == smin)
  assert(string.unpack("i" .. size, string.pack("i" .. size, smax)) == smax)
  assert(string.unpack("I" .. size, string.pack("I" .. size, umax)) == umax)
  assert(fails(string.pack, "i" .. size, smin - 1))
  assert(fails(string.pack, "i" .. size, smax + 1))
  assert(fails(string.pack, "I" .. size, umax + 1))
end
for size = 7, 16 do
  assert(string.unpack("i" .. size,
         string.pack("i" .. size, math.mininteger)) == math.mininteger)
  assert(string.unpack("I" .. size,
         string.pack("I" .. size, math.maxinteger)) == math.maxinteger)
end
assert(string.pack(">I2", 0x1234) == "\x12\x34")
assert(string.pack("<I2", 0x1234) == "\x34\x12")
assert(fails(string.pack, "b", 128) and fails(string.pack, "B", -1))
assert(fails(string.pack, "I1", 256) and fails(string.pack, "i1", -129))
for _, spec in ipairs({
  { "bB", -1, 1 }, { "hH", -1, 1 }, { "lL", -1, 1 },
  { "jJ", -1, 1 }, { "T", 1 }, { "i8I8", -1, 1 }
}) do
  local fmt = spec[1]
  local args = { unpack(spec, 2) }
  local packed = string.pack(fmt, unpack(args))
  local out = { string.unpack(fmt, packed) }
  assert(out[#out] == #packed + 1)
  for i = 1, #args do assert(out[i] == args[i]) end
end

assert(string.pack("c0", "") == "")
assert(string.pack("c3", "a\0") == "a\0\0")
local fixed, nextpos = string.unpack("c3", "a\0b")
assert(fixed == "a\0b" and nextpos == 4)
local counted = string.pack("<s1s2", "", "a\0b")
local s1, s2, after = string.unpack("<s1s2", counted)
assert(s1 == "" and s2 == "a\0b" and after == #counted + 1)
local z = string.pack("z", "hello")
assert(z == "hello\0" and ({ string.unpack("z", z) })[1] == "hello")
assert(fails(string.pack, "z", "a\0b"))
assert(fails(string.unpack, "z", "unterminated"))
assert(fails(string.packsize, "s1") and fails(string.packsize, "z"))

assert(string.packsize("!4 bXi4i4") == 8)
local aligned = string.pack("!4 bXi4i4", 1, 2)
assert(#aligned == 8 and aligned:sub(2, 4) == "\0\0\0")
local av, ai, ap = string.unpack("!4 bXi4i4", aligned)
assert(av == 1 and ai == 2 and ap == 9)
assert(#string.pack("bxB", 1, 2) == 3)
assert(string.pack("= B B", 1, 2) == "\1\2")
assert(fails(string.packsize, "!3i4"))
assert(fails(string.packsize, "X") and fails(string.packsize, "Xc1"))
assert(fails(string.packsize, "i0") and fails(string.packsize, "i17"))
assert(fails(string.packsize, "c") and fails(string.packsize, "?"))
local tail, tailpos = string.unpack("B", "abc", -1)
assert(tail == string.byte("c") and tailpos == 4)
assert(({ string.unpack("", "abc", 4) })[1] == 4)
assert(fails(string.unpack, "", "abc", 0))
assert(fails(string.unpack, "", "abc", 1.5))
assert(fails(string.unpack, "I4", "x"))
assert(fails(string.unpack, "I8", string.rep("\255", 8)))
assert(fails(string.unpack, string.rep("B", 8000), string.rep("\0", 8000)))

for _, fmt in ipairs({ "f", "d", "n", "<f", ">d" }) do
  for _, v in ipairs({ 1.5, 1/0, -1/0 }) do
    local got = string.unpack(fmt, string.pack(fmt, v))
    assert(got == v)
  end
  local negzero = string.unpack(fmt, string.pack(fmt, -0.0))
  assert(negzero == 0 and 1 / negzero == -1/0)
  local nan = string.unpack(fmt, string.pack(fmt, 0/0))
  assert(nan ~= nan)
end

local scalars = { 0, 0x7f, 0x80, 0x7ff, 0x800, 0xd7ff,
                  0xe000, 0xffff, 0x10000, 0x10ffff }
local encoded = utf8.char(unpack(scalars))
assert(utf8.len(encoded) == #scalars)
local decoded = { utf8.codepoint(encoded, 1, -1) }
for i = 1, #scalars do assert(decoded[i] == scalars[i]) end
local positions, iterated = {}, {}
for pos, cp in utf8.codes(encoded) do
  positions[#positions + 1], iterated[#iterated + 1] = pos, cp
end
assert(#positions == #scalars)
for i = 1, #scalars do assert(iterated[i] == scalars[i]) end
assert(utf8.len(encoded, -4, -1) == 1)
assert(utf8.len(encoded, #encoded + 1, #encoded) == 0)
assert(select("#", utf8.codepoint(encoded, 2, 1)) == 0)
assert(utf8.offset(encoded, 1) == 1)
assert(utf8.offset(encoded, -1) == positions[#positions])
assert(utf8.offset(encoded, 0, positions[4] + 1) == positions[4])
assert(utf8.offset(encoded, #scalars + 1) == #encoded + 1)
assert(utf8.offset(encoded, #scalars + 2) == nil)
assert(fails(utf8.offset, encoded, 1, positions[4] + 1))
assert(utf8.charpattern == "[\0-\x7f\xc2-\xf4][\x80-\xbf]*")
assert(fails(utf8.char, -1) and fails(utf8.char, 0xd800) and
       fails(utf8.char, 0xdfff) and fails(utf8.char, 0x110000) and
       fails(utf8.char, 1.5))

local malformed = {
  "\128", "\191", "\192\128", "\193\191",
  "\224\128\128", "\240\128\128\128",
  "\237\160\128", "\237\191\191", "\244\144\128\128",
  "\245\128\128\128", "\194", "\224", "\224\160",
  "\240", "\240\144", "\240\144\128",
  "\194A", "\224\160A", "\240\144\128A"
}
for _, bad in ipairs(malformed) do
  local n, pos = utf8.len(bad)
  assert(n == nil and pos == 1)
  assert(fails(utf8.codepoint, bad))
  assert(fails(function() for _ in utf8.codes(bad) do end end))
  local prefixed = "A" .. bad
  local pn, ppos = utf8.len(prefixed)
  assert(pn == nil and ppos == 2)
  ok, err = pcall(utf8.codepoint, prefixed, 1, -1)
  assert(not ok and tostring(err):find("byte 2", 1, true))
  ok, err = pcall(function() for _ in utf8.codes(prefixed) do end end)
  assert(not ok and tostring(err):find("byte 2", 1, true))
end

print("compat Lua 5.3 stdlib passed")
