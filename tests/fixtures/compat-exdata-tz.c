#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define CHECK(c) do { \
  if (!(c)) { \
    fprintf(stderr, "compat-exdata-tz check failed at line %d: %s\n", \
            __LINE__, #c); \
    exit(1); \
  } \
} while (0)

static int marker_a, marker_b;

typedef struct ExdataStress {
  lua_State *L;
  _Atomic int done;
} ExdataStress;

typedef struct ExdataDelayedStore {
  lua_State *L;
  void *value;
} ExdataDelayedStore;

static void *stress_writer(void *arg)
{
  ExdataStress *stress = (ExdataStress *)arg;
  int i;
  for (i = 0; i < 250000; i++)
    lua_setexdata(stress->L, (i & 1) ? &marker_a : &marker_b);
  atomic_store_explicit(&stress->done, 1, memory_order_release);
  return NULL;
}

static void *delayed_writer(void *arg)
{
  ExdataDelayedStore *store = (ExdataDelayedStore *)arg;
#if !defined(_WIN32)
  struct timespec delay = { 0, 50000000 };
  nanosleep(&delay, NULL);
#endif
  lua_setexdata(store->L, store->value);
  return NULL;
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
  CHECK(lua_gettop(L) == base + results);
}

static void test_exdata_c(lua_State *L, int lua_surface)
{
  pthread_t writer;
  ExdataStress stress;
  ExdataDelayedStore delayed;
  lua_State *child;
  CHECK(lua_getexdata(L) == NULL);
  lua_setexdata(L, &marker_a);
  CHECK(lua_getexdata(L) == &marker_a);
  child = lua_newthread(L);
  CHECK(child != NULL && lua_getexdata(child) == &marker_a);
  lua_setexdata(L, &marker_b);
  CHECK(lua_getexdata(L) == &marker_b);
  CHECK(lua_getexdata(child) == &marker_a);
  lua_setexdata(child, NULL);
  CHECK(lua_getexdata(child) == NULL && lua_getexdata(L) == &marker_b);
  lua_pop(L, 1);

  stress.L = L;
  atomic_init(&stress.done, 0);
  CHECK(pthread_create(&writer, NULL, stress_writer, &stress) == 0);
  while (!atomic_load_explicit(&stress.done, memory_order_acquire)) {
    void *value = lua_getexdata(L);
    CHECK(value == &marker_a || value == &marker_b);
  }
  CHECK(pthread_join(writer, NULL) == 0);

  if (lua_surface) {
    lua_settop(L, 0);
    lua_setexdata(L, &marker_a);
    run_chunk(L,
      "local exdata = require('thread.exdata') "
      "jit.flush() jit.opt.start('hotloop=1') "
      "function compat_poll_exdata(limit) "
      "  local initial = exdata() "
      "  for i=1,limit do if exdata() ~= initial then return true end end "
      "  return false "
      "end "
      "assert(compat_poll_exdata(1000) == false)", 0);
    delayed.L = L;
    delayed.value = &marker_b;
    CHECK(pthread_create(&writer, NULL, delayed_writer, &delayed) == 0);
    run_chunk(L, "return compat_poll_exdata(100000000)", 1);
    CHECK(lua_toboolean(L, -1));
    lua_pop(L, 1);
    CHECK(pthread_join(writer, NULL) == 0);
  }
  lua_setexdata(L, NULL);
}

#if !defined(_WIN32)
static const char *date_hour(lua_State *L)
{
  const char *hour;
  lua_getglobal(L, "os");
  lua_getfield(L, -1, "date");
  lua_pushliteral(L, "%H");
  lua_pushnumber(L, 0);
  lua_call(L, 2, 1);
  hour = lua_tostring(L, -1);
  CHECK(hour != NULL);
  return hour;
}

static void test_timezone_refresh(lua_State *L)
{
  const char *old = getenv("TZ");
  char *saved = old ? strdup(old) : NULL;
  const char *hour;
  CHECK(setenv("TZ", "UTC0", 1) == 0);
  hour = date_hour(L);
  CHECK(strcmp(hour, "00") == 0);
  lua_settop(L, 0);
  CHECK(setenv("TZ", "EST5", 1) == 0);
  hour = date_hour(L);
  CHECK(strcmp(hour, "19") == 0);
  lua_settop(L, 0);
  if (saved) {
    CHECK(setenv("TZ", saved, 1) == 0);
    free(saved);
  } else {
    CHECK(unsetenv("TZ") == 0);
  }
  tzset();
}
#endif

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  CHECK(L != NULL);
  luaL_openlibs(L);
  if (argc == 1 || strcmp(argv[1], "exdata") == 0 ||
      strcmp(argv[1], "exdata-basic") == 0)
    test_exdata_c(L, argc == 1 || strcmp(argv[1], "exdata") == 0);
#if !defined(_WIN32)
  if (argc == 1 || strcmp(argv[1], "tz") == 0)
    test_timezone_refresh(L);
#else
  if (argc == 1 || strcmp(argv[1], "tz") == 0)
    puts("compat timezone refresh skipped on Windows");
#endif
  lua_close(L);
  puts("compat exdata and timezone passed");
  return 0;
}
