#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#define CHECK(c) do { \
  if (!(c)) { \
    fprintf(stderr, "compat-api52 check failed at line %d: %s\n", \
            __LINE__, #c); \
    exit(1); \
  } \
} while (0)

static int opener_calls;

static int compat_len(lua_State *L)
{
  lua_pushliteral(L, "7");
  return 1;
}

static int compat_opener(lua_State *L)
{
  size_t len;
  const char *name = lua_tolstring(L, 1, &len);
  CHECK(name != NULL && len == strlen("compat.fixture") &&
        memcmp(name, "compat.fixture", len) == 0);
  opener_calls++;
  lua_pushlstring(L, "module\0value", 12);
  return 1;
}

static void run_chunk(lua_State *L, const char *code, int results)
{
  int base = lua_gettop(L);
  int status = luaL_loadstring(L, code);
  if (status == 0)
    status = lua_pcall(L, 0, results, 0);
  if (status != 0) {
    fprintf(stderr, "Lua chunk failed: %s\n", lua_tostring(L, -1));
    exit(1);
  }
  CHECK(results == LUA_MULTRET || lua_gettop(L) == base + results);
}

static void test_absindex(lua_State *L)
{
  lua_settop(L, 0);
  lua_pushnumber(L, 1);
  lua_pushnumber(L, 2);
  CHECK(lua_absindex(L, 1) == 1);
  CHECK(lua_absindex(L, -1) == 2);
  CHECK(lua_absindex(L, LUA_REGISTRYINDEX) == LUA_REGISTRYINDEX);
  CHECK(lua_absindex(L, LUA_GLOBALSINDEX) == LUA_GLOBALSINDEX);
  CHECK(lua_gettop(L) == 2);
  lua_pushglobaltable(L);
  lua_getglobal(L, "_G");
  CHECK(lua_rawequal(L, -1, -2));
  lua_pop(L, 2);
}

static void test_lengths(lua_State *L)
{
  int top;
  lua_settop(L, 0);
  lua_pushlstring(L, "a\0b", 3);
  CHECK(lua_rawlen(L, -1) == 3 && lua_objlen(L, -1) == 3);
  lua_pop(L, 1);

  lua_pushnumber(L, 12345);
  CHECK(lua_rawlen(L, -1) == 0);
  CHECK(lua_objlen(L, -1) == 5);
  lua_pop(L, 1);

  lua_createtable(L, 2, 0);
  lua_pushboolean(L, 1); lua_rawseti(L, -2, 1);
  lua_pushboolean(L, 1); lua_rawseti(L, -2, 2);
  CHECK(lua_rawlen(L, -1) == 2);
  CHECK(luaL_len(L, -1) == 2);
  lua_pop(L, 1);

  (void)lua_newuserdata(L, 17);
  lua_newtable(L);
  lua_pushcfunction(L, compat_len);
  lua_setfield(L, -2, "__len");
  lua_setmetatable(L, -2);
  CHECK(lua_rawlen(L, -1) == 17);
  top = lua_gettop(L);
  lua_len(L, -1);
  CHECK(lua_gettop(L) == top + 1 && lua_type(L, -1) == LUA_TSTRING &&
        strcmp(lua_tostring(L, -1), "7") == 0);
  lua_pop(L, 1);
  CHECK(luaL_len(L, -1) == 7 && lua_gettop(L) == top);
  lua_pop(L, 1);

  run_chunk(L, "return setmetatable({1,2}, {__len=function() return 'meta' end})", 1);
  top = lua_gettop(L);
  lua_len(L, -1);
  CHECK(lua_gettop(L) == top + 1);
  if (luaJIT_compat52) {
    CHECK(lua_type(L, -1) == LUA_TSTRING &&
          strcmp(lua_tostring(L, -1), "meta") == 0);
  } else {
    CHECK(lua_type(L, -1) == LUA_TNUMBER && lua_tointeger(L, -1) == 2);
  }
  lua_settop(L, 0);
}

static void test_tolstring(lua_State *L)
{
  size_t len;
  const char *s;
  lua_settop(L, 0);

  lua_pushlstring(L, "a\0b", 3);
  s = luaL_tolstring(L, -1, &len);
  CHECK(s != NULL && len == 3 && memcmp(s, "a\0b", 3) == 0);
  lua_settop(L, 0);

  lua_pushnumber(L, 42);
  s = luaL_tolstring(L, -1, &len);
  CHECK(s != NULL && len == 2 && memcmp(s, "42", 2) == 0);
  lua_settop(L, 0);
  lua_pushboolean(L, 1);
  CHECK(strcmp(luaL_tolstring(L, -1, NULL), "true") == 0);
  lua_settop(L, 0);
  lua_pushnil(L);
  CHECK(strcmp(luaL_tolstring(L, -1, NULL), "nil") == 0);
  lua_settop(L, 0);
  lua_newtable(L);
  s = luaL_tolstring(L, -1, &len);
  CHECK(s != NULL && len > 7 && memcmp(s, "table: ", 7) == 0);
  lua_settop(L, 0);

  run_chunk(L, "return setmetatable({}, {__tostring=function() return 42 end})", 1);
  CHECK(strcmp(luaL_tolstring(L, -1, NULL), "42") == 0);
  lua_settop(L, 0);
  run_chunk(L, "return setmetatable({}, {__tostring=function() return 'meta' end})", 1);
  CHECK(strcmp(luaL_tolstring(L, -1, NULL), "meta") == 0);
  lua_settop(L, 0);
  run_chunk(L, "return setmetatable({}, {__tostring=function() return {} end})", 1);
  CHECK(luaL_tolstring(L, -1, &len) == NULL);
  CHECK(lua_type(L, -1) == LUA_TTABLE);
  lua_settop(L, 0);
}

static void test_subtable_require_buffer(lua_State *L)
{
  luaL_Buffer b;
  char *p;
  size_t len;
  lua_settop(L, 0);
  lua_newtable(L);
  CHECK(luaL_getsubtable(L, -1, "missing") == 0);
  CHECK(lua_istable(L, -1));
  lua_pop(L, 1);
  CHECK(luaL_getsubtable(L, -1, "missing") == 1);
  lua_pop(L, 1);
  lua_pushnumber(L, 1); lua_setfield(L, -2, "wrong");
  CHECK(luaL_getsubtable(L, -1, "wrong") == 0);
  CHECK(lua_istable(L, -1));
  lua_settop(L, 0);

  lua_pushnil(L); lua_setglobal(L, "package");
  luaL_requiref(L, "compat.fixture", compat_opener, 1);
  CHECK(opener_calls == 1 && lua_gettop(L) == 1);
  CHECK(lua_tolstring(L, -1, &len) != NULL && len == 12);
  lua_pop(L, 1);
  luaL_requiref(L, "compat.fixture", compat_opener, 0);
  CHECK(opener_calls == 1 && lua_gettop(L) == 1);
  lua_getglobal(L, "compat.fixture");
  CHECK(lua_rawequal(L, -1, -2));
  lua_settop(L, 0);

  luaL_buffinit(L, &b);
  p = luaL_prepbuffer(&b);
  memcpy(p, "a\0b", 3);
  luaL_pushresultsize(&b, 3);
  CHECK(lua_gettop(L) == 1);
  p = (char *)lua_tolstring(L, -1, &len);
  CHECK(p != NULL && len == 3 && memcmp(p, "a\0b", 3) == 0);
  lua_settop(L, 0);
}

static void test_utf8_library(lua_State *L)
{
  int top;
  lua_settop(L, 0);
  lua_getglobal(L, "utf8");
  CHECK(lua_istable(L, -1));
  lua_getfield(L, -1, "char");
  CHECK(lua_isfunction(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, -1, "charpattern");
  CHECK(lua_type(L, -1) == LUA_TSTRING && lua_rawlen(L, -1) != 0);
  lua_settop(L, 0);
  top = lua_gettop(L);
  CHECK(luaopen_utf8(L) == 1 && lua_gettop(L) == top + 1 &&
        lua_istable(L, -1));
  lua_settop(L, 0);
}

int main(int argc, char **argv)
{
  int expected_compat52 = argc > 1 ? atoi(argv[1]) : -1;
  lua_State *L = luaL_newstate();
  CHECK(L != NULL);
  luaL_openlibs(L);
  CHECK(luaJIT_compat52 == 0 || luaJIT_compat52 == 1);
  if (expected_compat52 >= 0)
    CHECK(luaJIT_compat52 == expected_compat52);
  test_absindex(L);
  test_lengths(L);
  test_tolstring(L);
  test_subtable_require_buffer(L);
  test_utf8_library(L);
  lua_close(L);
  puts("compat API 5.2 passed");
  return 0;
}
