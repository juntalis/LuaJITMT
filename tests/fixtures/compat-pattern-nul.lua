local function fails(fn, ...)
  return not pcall(fn, ...)
end

local nul = "\0"
assert(string.match(nul .. "ab", nul) == nul)
assert(string.match("a" .. nul .. "b", "a" .. nul .. "b") == "a" .. nul .. "b")
assert(string.match("ab" .. nul, nul .. "$") == nul)
assert(string.match("a" .. nul .. nul .. "b", "a" .. nul .. "+b") ==
       "a" .. nul .. nul .. "b")
assert(string.match("a" .. nul, "[x" .. nul .. "]$") == nul)
assert(string.match("a" .. nul .. "b", "(a" .. nul .. "b)") ==
       "a" .. nul .. "b")
assert(string.match("a" .. nul .. "a" .. nul, "(." .. nul .. ")%1") ==
       "a" .. nul)
assert(string.match(nul .. "x", "%f[x]") == "")
assert(string.match("a" .. nul, "a" .. nul .. "$") == "a" .. nul)

local replaced, count = string.gsub("a" .. nul .. "a", nul, "x")
assert(replaced == "axa" and count == 1)
replaced, count = string.gsub("a" .. nul .. "a", "^a" .. nul, "z")
assert(replaced == "za" and count == 1)
replaced, count = string.gsub("a" .. nul .. "a", "^" .. nul, "z")
assert(replaced == "a" .. nul .. "a" and count == 0)

local seen = 0
for _ in string.gmatch("ab", nul .. "*") do
  seen = seen + 1
  assert(seen <= 3)
end
assert(seen == 3)

assert(fails(string.match, "x", "%"))
assert(fails(string.match, "x", "["))
assert(fails(string.match, "x", "[%"))
assert(fails(string.match, "x", "%b"))
assert(fails(string.match, "x", "%b("))
assert(fails(string.match, "x", "%f"))
assert(fails(string.match, "x", "%f["))

print("compat pattern NUL passed")
