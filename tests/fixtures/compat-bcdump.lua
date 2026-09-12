local control = "line\nvalue\0"
local number = 123.25
local table_value = { marker = true }

local function child(x)
  return x, control, number, table_value
end

return child(7)
