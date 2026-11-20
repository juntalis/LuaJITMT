# Build-related issues

Created: 2026-11-09, from the `moonjitmt` branch review (base `4f70a776e`).
Depends on: [16_build_strategy.md](16_build_strategy.md). Do not start until
that document has a decision recorded. Each item says which outcome makes it
obsolete.

## B1. CMake targets do not declare `Synchronization.lib`

- **Where:** `CMakeLists.txt` (`luajit-5.1`, `luajit-5.1-shared`).
- **Problem:** `src/lj_atomic.h` (`la_futex_wait`) calls `WaitOnAddress`.
  Only the `luajit` executable links `Synchronization.lib`. The DLL and any
  consumer of the static library get unresolved symbols on a fresh configure.
  The local `build-msvcrt` links only because it adds `synchronization.lib` from
  outside the committed file.
- **Fix:** `target_link_libraries(luajit-5.1 PUBLIC Synchronization)` and
  `target_link_libraries(luajit-5.1-shared PRIVATE Synchronization)` under
  `if(WIN32)`. Alternatively add `#pragma comment(lib, "synchronization.lib")`
  next to the call, matching how `lj_prng.c` handles `bcrypt.lib`.
- **Check:** configure a new build directory with no cache and build
  `luajit-5.1-shared`.
- **Obsolete if:** CMake is removed.

## B2. CMake does not reject unsupported platforms

- **Where:** `CMakeLists.txt:46-55`, `:151-157`.
- **Problem:** DynASM always gets `-D WIN` and buildvm always runs `-m peobj`.
  On Linux or macOS, configure succeeds and builds a VM with the wrong ABI.
- **Fix (minimal):** `message(FATAL_ERROR ...)` unless `WIN32` and the
  processor is x86-64.
- **Fix (full):** derive DynASM flags and the `-m` mode from `lj_arch.h` with a
  preprocessor probe, as `src/Makefile` does.
- **Obsolete if:** CMake is removed. Superseded by the full fix if CMake becomes
  canonical.

## B3. DynASM flags hard-coded in CMake and `configure.lua`

- **Where:** `CMakeLists.txt:46-55`, `tools/build/configure.lua:154-162`.
- **Problem:** both omit flags the Makefile adds conditionally (`NO_UNWIND`,
  `BRANCH_TRACK`, `SHADOW_STACK`, `DUALNUM`) and fix `VER=21`.
- **Fix:** whichever generator survives must probe `lj_arch.h` with the target
  compiler.
- **Obsolete if:** the corresponding generator is removed.

## B4. Generated files written into `src/`

- **Where:** `CMakeLists.txt:90`, `:144-149`.
- **Problem:** CMake and Make overwrite each other's `lj_bcdef.h`,
  `lj_ffdef.h`, `lj_libdef.h`, `lj_recdef.h`, `lj_folddef.h`, `luajit.h`
  and `jit/vmdef.lua`. Parallel build directories race on them.
- **Fix:** generate into the build directory. Because sources use quoted
  includes, stale copies in `src/` would still win, so the build must also fail
  (or warn) when it finds them.
- **Obsolete if:** only one build system remains and it always writes to
  `src/` (as upstream Make does).

## B5. Windows unwind carrier guarded inconsistently (uncommitted change)

- **Where:** `src/lj_tg.h` (field `err_win_jit_carrier` and its size assert),
  `src/lj_err.c:573-616`.
- **Problem:** the field is guarded by `LJ_TARGET_WINDOWS && LJ_TARGET_X64 &&
  LJ_UNWIND_JIT`, but the code using it is inside `#if LJ_ABI_WIN`. Cygwin x64
  has `LJ_ABI_WIN` without `LJ_TARGET_WINDOWS`, so it fails to compile. The
  removed `LJ_ERR_TLS` macro handled that case.
- **Fix:** guard the field and the assert with `LJ_ABI_WIN && LJ_TARGET_X64 &&
  LJ_UNWIND_JIT`.
- **Also:** `G2TG()` (`lj_thr_get_tg_fallback`) can return NULL. The old TLS
  carrier could not. Add `lj_assertG(tg != NULL, ...)` in
  `err_unwind_win_jit`.
- **Note:** listed here because it came in with the MSVC/DLL build work; it is
  a source change and applies to every build system.

## B6. Header guard mismatch for `luaopen_thread_exdata`

- **Where:** `src/lualib.h:39`, `src/lib_threading.c` (definition).
- **Problem:** the declaration is guarded by `!defined(LUAJIT_DISABLE_FFI)`,
  the definition by `LJ_HASFFI`. On an architecture without FFI support the
  symbol is declared and listed in `lib_init.c` but not defined.
- **Fix:** use one condition for the declaration, the definition and the
  `lib_init.c` entry.

## B7. pkg-config metadata

- **Where:** `CMakeLists.txt:105-114`, `cmake/luajit.pc.in`.
- **Problem:** `Version` is parsed from `luajit_rolling.h` and may not be a
  valid pkg-config version. `Libs.private: -lm -ldl -pthread` is wrong for the
  Windows-only build. There is no `install()` rule, so the file is never
  installed.
- **Fix:** take the version from `luajit_relver.txt` the way `genversion.lua`
  does, and set `Libs.private` per platform. Add `install()` rules or drop the
  `.pc` generation.
- **Obsolete if:** CMake is removed.

## B8. Local MSVC runtime experiment in `win/`

- **Where:** untracked `win/` (`msvcrt.lib`, `stdio_legacy`, `dll_main.c`),
  commented-out lines in `CMakeLists.txt:323,326`.
- **Problem:** the working local build depends on these files, but the
  repository does not reference them. The commented `/ENTRY:DllMain` lines
  suggest an unfinished approach.
- **Action:** decide under question 5 of the strategy document; then either
  commit and wire them in behind an option, or remove the commented lines.

## Order

B5 and B6 are independent of the strategy decision and can be done any time.
The rest wait for [16_build_strategy.md](16_build_strategy.md).
