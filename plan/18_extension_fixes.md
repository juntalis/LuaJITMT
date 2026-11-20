# MoonJIT extension fixes (non-build)

Created: 2026-11-09, from the `moonjitmt` branch review (base `4f70a776e`).
Scope: defects in the code added by
[MOONJIT_EXTRACTION_IMPLEMENTATION_PLAN.md](MOONJIT_EXTRACTION_IMPLEMENTATION_PLAN.md).
Independent of [16_build_strategy.md](16_build_strategy.md).

The project rules in `AGENTS.md` and
[MOONJIT_EXTRACTION_IMPLEMENTATION_PLAN.md](MOONJIT_EXTRACTION_IMPLEMENTATION_PLAN.md)
apply: use the current claim/rooting patterns in `lj_api.c`, no hot-path locks,
and tests must check public behavior. Put new tests in
`tests/suites/compat_extensions.lua` under the existing focused case names.

Items marked **HOLD** need a discussion with the maintainer before any change.

---

## E1. `lua_strlen` maps to `lua_rawlen` — HOLD

- **Where:** `src/lua.h:273`; `lua_rawlen` in `src/lj_api.c`.
- **Problem:** `lua_strlen` used to call `lua_objlen`, which converts a number
  to a string and returns its length. `lua_rawlen` returns 0 for numbers.
  Existing 5.1 C modules that call `lua_strlen` before `lua_tostring` get 0.
- **Maintainer direction:** revert to `lua_objlen` by default, and use the
  current `lua_rawlen` mapping when Lua 5.2 compatibility is enabled.
- **To discuss:**
  - The library's switch is `LUAJIT_ENABLE_LUA52COMPAT` (which sets `LJ_52`
    in `lj_arch.h`). `LUA52_COMPAT` does not exist in this tree. Confirm which
    name is meant.
  - `lua_strlen` is a macro, so it expands in the **consumer's** compilation.
    `LJ_52` is not visible from `lua.h`, and C modules are not built with
    `LUAJIT_ENABLE_LUA52COMPAT`. Options:
    1. Test `LUAJIT_ENABLE_LUA52COMPAT` in `lua.h`. The consumer must define
       it to match the library; nothing enforces that.
    2. Have the generated `luajit.h`/`luaconf.h` record the library's setting
       at build time, and test that in `lua.h`.
    3. Keep the macro pointing to an exported function that checks `LJ_52`
       inside the library. This always matches the library build.
- **Tests:** a C fixture that pushes a number and checks `lua_strlen` in both
  build profiles.

## E2. Public `lua_Unsigned` typedef

- **Where:** `src/lua.h:107`.
- **Problem:** only the `'J'` option in `lib_string.c` uses it. lua-compat-5.3
  defines its own `lua_Unsigned` for Lua 5.1 headers, and a repeated typedef is
  an error in C99.
- **Fix:** remove it from `lua.h`; use `size_t` or a private typedef in
  `lib_string.c`.
- **Tests:** compile a fixture that includes `lua.h` then declares
  `typedef size_t lua_Unsigned;` with `-std=c99 -pedantic-errors`.

## E3. `threading.exdata` setter available to any Lua code — HOLD

- **Where:** `threading_exdata` in `src/lib_threading.c`; recorder in
  `src/lj_ffrecord.c`.
- **Problem:** `threading.exdata(nil)` works without the FFI module and clears
  the host's pointer. `threading` is always preloaded, so a sandbox cannot block
  it. With FFI loaded, a script can also set an arbitrary pointer that the host
  will dereference.
- **Options to discuss:**
  1. Lua side read-only; setter only through `lua_setexdata`.
  2. Keep the setter, but only when the host opts in (for example, a flag set
     through the C API).
  3. Keep as is and document that hosts must not expose `threading` to
     untrusted code.
- **Tests:** whichever is chosen, a case under `compat_exdata` with `-joff` and
  `-jon`.

## E4. `math.maxinteger` is 2^53 — HOLD

- **Where:** `MATH_INT53_MAX` in `src/lib_math.c:103`; the same limit is
  repeated as `PACK_INT_EXACT_MAX` in `src/lib_string.c` and inline in
  `utf8_checkinteger` in `src/lib_utf8.c`.
- **Problem:** `math.maxinteger + 1 == math.maxinteger` is true (confirmed).
  2^53 is exactly representable, but its successor is not.
- **Proposal to discuss:** use 2^53−1 (and −(2^53−1)) everywhere, defined once
  in a shared header. Points to settle:
  - whether `math.type(2^53)` should then be `"float"`;
  - whether `string.pack("j", 2^53)` should then fail;
  - whether to match MoonJIT's existing behavior instead.

## E5. `utf8` installed as a global

- **Where:** `src/lib_init.c:30`; `luaopen_utf8` in `src/lib_utf8.c`.
- **Problem:** `luaL_register` sets the global `utf8` and
  `package.loaded.utf8`. `require "utf8"` then returns the built-in instead of
  a user's `utf8.lua`, and code that checks for `utf8` to detect Lua 5.3 takes
  the wrong branch.
- **Fix:** move it to `lj_lib_preload` so it is available through
  `require "utf8"` but not a global. Optionally make it a global only when
  `LJ_52` is set.
- **Note:** `package.preload` is searched before `package.path`, so a
  preloaded `utf8` still shadows a user's `utf8.lua`. Moving it to preload
  fixes the global and feature-detection problems only. If shadowing also
  matters, the module needs a different name (for example `lj.utf8`).
- **Tests:** `utf8 == nil` and `require("utf8").char(65) == "A"` by default.

## E6. `luaL_tolstring` accepts a non-string `__tostring` result

- **Where:** `luaL_tolstring` in `src/lib_aux.c`.
- **Problem:** if `__tostring` returns a table, the function returns NULL.
  Callers expect a string.
- **Fix:** after `luaL_callmeta`, `if (!lua_isstring(L, -1)) luaL_error(L,
  "'__tostring' must return a string");` (Lua 5.3 behavior).
- **Tests:** C fixture in `compat-api52.c`.

## E7. `utf8` argument handling differs from Lua 5.3

- **Where:** `utf8_checkinteger` and `utf8_char` in `src/lib_utf8.c`.
- **Problems:**
  - Numeric strings are rejected: `utf8.char("65")` errors (confirmed); Lua 5.3
    accepts it.
  - `utf8.char(0xD800)` is rejected; Lua 5.3 accepts surrogates.
- **Fix:** accept numeric strings (use `lua_isnumber`). For surrogates, either
  accept them (5.3) or keep the strict check and document it.
- **Tests:** add both calls to `compat-stdlib53.lua`.

## E8. `os.date` calls `tzset()` on every local-time call

- **Where:** `os_date_tm` in `src/lib_os.c`.
- **Problem:** glibc's `tzset()` takes a global lock and checks
  `/etc/localtime` each time. It also races with `setenv` in other threads.
- **Fix:** call `tzset()` only when `getenv("TZ")` differs from the last value
  seen (stored per thread or compared atomically), or document the cost.
- **Tests:** the existing `compat_os_date_tz` case must still pass.

## E9. `luaopen_thread_exdata` depends on the global `require`

- **Where:** `luaopen_thread_exdata` in `src/lib_threading.c`.
- **Problem:** fails if a sandbox removes `require`.
- **Fix:** read `threading` from the registry `_LOADED` table, or open it
  directly, without going through globals.
- **Depends on:** E3's outcome; if the Lua-side setter is removed, this module
  may change shape.

## E10. `luaJIT_compat52` is writable

- **Where:** `src/lj_api.c:36`, `src/luajit_rolling.h`.
- **Fix:** declare and define it `const int`. Check that the Windows DLL export
  still works with `dllimport` on a `const` data symbol.

---

## Order

1. E2, E5, E6, E7, E10 — agreed; independent of each other.
2. E8, E9 — agreed; E9 after E3 is decided.
3. E1, E3, E4 — after discussion.
