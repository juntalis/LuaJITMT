/*
** UTF-8 library (Lua 5.3 compatible surface).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <limits.h>
#include <math.h>
#include <stdint.h>

#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* ------------------------------------------------------------------------ */

static int64_t utf8_checkinteger(lua_State *L, int arg)
{
  lua_Number n;
  if (lua_type(L, arg) != LUA_TNUMBER)
    luaL_typerror(L, arg, "number");
  n = lua_tonumber(L, arg);
  if (n != n || n < -9007199254740992.0 ||
      n > 9007199254740992.0 || floor(n) != n)
    luaL_argerror(L, arg, "number has no integer representation");
  return (int64_t)n;
}

static int64_t utf8_optinteger(lua_State *L, int arg, int64_t def)
{
  return lua_isnoneornil(L, arg) ? def : utf8_checkinteger(L, arg);
}

static int64_t utf8_posrelat(int64_t pos, size_t len)
{
  if (pos < 0) {
    uint64_t magnitude = (uint64_t)(-(pos + 1)) + 1u;
    if (magnitude > (uint64_t)len)
      return 0;
    pos += (int64_t)len + 1;
  }
  return pos;
}

/* Strict scalar decoder. Every read is preceded by a remaining-length check. */
static const unsigned char *utf8_decode(const unsigned char *s,
					 const unsigned char *end,
					 uint32_t *value)
{
  uint32_t c, res;
  if (s >= end)
    return NULL;
  c = *s++;
  if (c < 0x80) {
    *value = c;
    return s;
  }
  if (c >= 0xc2 && c <= 0xdf) {
    if (end - s < 1 || s[0] < 0x80 || s[0] > 0xbf)
      return NULL;
    *value = ((c & 0x1f) << 6) | (s[0] & 0x3f);
    return s + 1;
  }
  if (c >= 0xe0 && c <= 0xef) {
    if (end - s < 2 || s[0] < 0x80 || s[0] > 0xbf ||
	s[1] < 0x80 || s[1] > 0xbf ||
	(c == 0xe0 && s[0] < 0xa0) || (c == 0xed && s[0] > 0x9f))
      return NULL;
    res = ((c & 0x0f) << 12) | ((s[0] & 0x3f) << 6) | (s[1] & 0x3f);
    *value = res;
    return s + 2;
  }
  if (c >= 0xf0 && c <= 0xf4) {
    if (end - s < 3 || s[0] < 0x80 || s[0] > 0xbf ||
	s[1] < 0x80 || s[1] > 0xbf || s[2] < 0x80 || s[2] > 0xbf ||
	(c == 0xf0 && s[0] < 0x90) || (c == 0xf4 && s[0] > 0x8f))
      return NULL;
    res = ((c & 0x07) << 18) | ((s[0] & 0x3f) << 12) |
	  ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
    *value = res;
    return s + 3;
  }
  return NULL;
}

static void utf8_invalid(lua_State *L, const unsigned char *start,
			 const unsigned char *at)
{
  luaL_error(L, "invalid UTF-8 code at byte %d", (int)(at - start) + 1);
}

static int utf8_char(lua_State *L)
{
  int i, nargs = lua_gettop(L);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (i = 1; i <= nargs; i++) {
    int64_t value = utf8_checkinteger(L, i);
    unsigned char out[4];
    size_t n;
    if (value < 0 || value > 0x10ffff ||
	(value >= 0xd800 && value <= 0xdfff))
      luaL_argerror(L, i, "value out of range");
    if (value < 0x80) {
      out[0] = (unsigned char)value;
      n = 1;
    } else if (value < 0x800) {
      out[0] = (unsigned char)(0xc0 | (value >> 6));
      out[1] = (unsigned char)(0x80 | (value & 0x3f));
      n = 2;
    } else if (value < 0x10000) {
      out[0] = (unsigned char)(0xe0 | (value >> 12));
      out[1] = (unsigned char)(0x80 | ((value >> 6) & 0x3f));
      out[2] = (unsigned char)(0x80 | (value & 0x3f));
      n = 3;
    } else {
      out[0] = (unsigned char)(0xf0 | (value >> 18));
      out[1] = (unsigned char)(0x80 | ((value >> 12) & 0x3f));
      out[2] = (unsigned char)(0x80 | ((value >> 6) & 0x3f));
      out[3] = (unsigned char)(0x80 | (value & 0x3f));
      n = 4;
    }
    luaL_addlstring(&b, (const char *)out, n);
  }
  luaL_pushresult(&b);
  return 1;
}

static int utf8_len(lua_State *L)
{
  size_t len;
  const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &len);
  const unsigned char *end = s + len;
  int64_t i = utf8_posrelat(utf8_optinteger(L, 2, 1), len);
  int64_t j = utf8_posrelat(utf8_optinteger(L, 3, -1), len);
  int64_t count = 0;
  luaL_argcheck(L, i >= 1 && (uint64_t)i <= (uint64_t)len + 1u,
		2, "initial position out of string");
  luaL_argcheck(L, j <= 0 || (uint64_t)j <= (uint64_t)len,
		3, "final position out of string");
  while (i <= j) {
    uint32_t value;
    const unsigned char *next = utf8_decode(s + (size_t)i - 1u, end, &value);
    if (next == NULL) {
      lua_pushnil(L);
      lua_pushinteger(L, i);
      return 2;
    }
    i = (int64_t)(next - s) + 1;
    count++;
  }
  lua_pushnumber(L, (lua_Number)count);
  return 1;
}

static int utf8_codepoint(lua_State *L)
{
  size_t len;
  const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &len);
  const unsigned char *end = s + len;
  int64_t i = utf8_posrelat(utf8_optinteger(L, 2, 1), len);
  int64_t j = utf8_posrelat(utf8_optinteger(L, 3, i), len);
  int64_t pos = i;
  size_t wanted;
  int results = 0;
  luaL_argcheck(L, i >= 1, 2, "out of range");
  luaL_argcheck(L, j <= 0 || (uint64_t)j <= (uint64_t)len,
		3, "out of range");
  if (i > j)
    return 0;
  wanted = (size_t)(j - i) + 1u;
  luaL_argcheck(L, wanted <= (size_t)INT_MAX, 2, "string slice too long");
  luaL_checkstack(L, (int)wanted, "too many results");
  while (pos <= j) {
    uint32_t value;
    const unsigned char *next = utf8_decode(s + (size_t)pos - 1u, end, &value);
    if (next == NULL)
      utf8_invalid(L, s, s + (size_t)pos - 1u);
    lua_pushinteger(L, (lua_Integer)value);
    results++;
    pos = (int64_t)(next - s) + 1;
  }
  return results;
}

static int utf8_codes_aux(lua_State *L)
{
  size_t len;
  const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &len);
  const unsigned char *end = s + len;
  int64_t control = utf8_checkinteger(L, 2);
  const unsigned char *at, *next;
  uint32_t value;
  if (control < 0)
    control = 0;
  if ((uint64_t)control > (uint64_t)len)
    return 0;
  if (control == 0) {
    at = s;
  } else {
    at = s + (size_t)control - 1u;
    next = utf8_decode(at, end, &value);
    if (next == NULL)
      utf8_invalid(L, s, at);
    at = next;
  }
  if (at >= end)
    return 0;
  next = utf8_decode(at, end, &value);
  if (next == NULL)
    utf8_invalid(L, s, at);
  lua_pushnumber(L, (lua_Number)(at - s) + 1);
  lua_pushinteger(L, (lua_Integer)value);
  return 2;
}

static int utf8_codes(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TSTRING);
  lua_settop(L, 1);
  lua_pushcfunction(L, utf8_codes_aux);
  lua_insert(L, 1);
  lua_pushinteger(L, 0);
  return 3;
}

static int utf8_offset(lua_State *L)
{
  size_t len;
  const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &len);
  int64_t n = utf8_checkinteger(L, 2);
  int64_t def = n >= 0 ? 1 : (int64_t)len + 1;
  int64_t pos = utf8_posrelat(utf8_optinteger(L, 3, def), len);
  luaL_argcheck(L, pos >= 1 && (uint64_t)pos <= (uint64_t)len + 1u,
		3, "position out of range");
  if (n == 0) {
    while (pos > 1 && (uint64_t)pos <= (uint64_t)len &&
	   (s[(size_t)pos - 1u] & 0xc0) == 0x80)
      pos--;
  } else {
    if ((uint64_t)pos <= (uint64_t)len &&
	(s[(size_t)pos - 1u] & 0xc0) == 0x80)
      luaL_error(L, "initial position is a continuation byte");
    if (n < 0) {
      while (n < 0 && pos > 1) {
	pos--;
	while (pos > 1 && (s[(size_t)pos - 1u] & 0xc0) == 0x80)
	  pos--;
	n++;
      }
    } else {
      n--;
      while (n > 0 && (uint64_t)pos <= (uint64_t)len) {
	pos++;
	while ((uint64_t)pos <= (uint64_t)len &&
	       (s[(size_t)pos - 1u] & 0xc0) == 0x80)
	  pos++;
	n--;
      }
    }
  }
  if (n == 0)
    lua_pushnumber(L, (lua_Number)pos);
  else
    lua_pushnil(L);
  return 1;
}

/* ------------------------------------------------------------------------ */

static const luaL_Reg utf8_lib[] = {
  { "char", utf8_char },
  { "codepoint", utf8_codepoint },
  { "codes", utf8_codes },
  { "len", utf8_len },
  { "offset", utf8_offset },
  { NULL, NULL }
};

LUALIB_API int luaopen_utf8(lua_State *L)
{
  static const char charpattern[] =
    "[\x00-\x7f\xc2-\xf4][\x80-\xbf]*";
  luaL_register(L, LUA_UTF8LIBNAME, utf8_lib);
  lua_pushlstring(L, charpattern, sizeof(charpattern) - 1u);
  lua_setfield(L, -2, "charpattern");
  return 1;
}
