# Build strategy

Created: 2026-11-09, from the `moonjitmt` branch review (base `4f70a776e`).
Status: **open for discussion — no decision recorded yet.**

This document must be resolved before work starts on
[17_build_issues.md](17_build_issues.md). Documents 18 and 19 do not depend on
it, but their tests run through whatever build path is chosen here.

## Why this exists

The tree now has three build systems that do overlapping jobs, and none of them
covers every supported target:

| System | Location | Targets in practice | Used by |
| --- | --- | --- | --- |
| GNU Make | `src/Makefile`, top-level `Makefile` | Linux, macOS, Windows via MinGW cross-compile (`CROSS=`) | `tools/ci/platform_build.sh`, `tests/lib/ljtest.lua` (`Test:make`), the MoonJIT plan's Milestone 0, stock LuaJIT docs |
| CMake | `CMakeLists.txt`, `cmake/luajit.pc.in` | Windows x64 with MSVC only (hard-coded `-D WIN`, `-m peobj`) | Local `build-msvcrt/` builds only |
| Lua → Ninja | `tools/build/configure.lua`, `ninja.lua`, `ninja_syntax.lua`, `minitest.lua` | Claims Windows/Linux/macOS with `msvc` or `gcc` toolkits | Nothing in CI or the test harness |

`src/msvcbuild.bat` no longer exists, so native MSVC has no build route except
CMake and `configure.lua`.

The MoonJIT extraction plan said: "Do not invent a new build system as part of
this work." The CMake and Ninja generators were added anyway, and they now
duplicate source lists and DynASM flags by hand.

## Known problems with the current arrangement

1. **Duplicated configuration drifts.** The Makefile derives DynASM flags
   (`NO_UNWIND`, `BRANCH_TRACK`, `SHADOW_STACK`, `DUALNUM`, `VER`, `WIN`) by
   preprocessing `lj_arch.h` with the target compiler (`TARGET_TESTARCH`).
   CMake and `configure.lua` hard-code a fixed list. A CET-enabled Linux
   toolchain, for example, would get a VM that disagrees with the C code.
2. **Source lists repeated three times.** `LJCORE_O`/`LJLIB_O` (Makefile),
   `LJCORE_C`/`LJLIB_C` (CMake) and the lists in `configure.lua` must change
   together. `lib_utf8.c` had to be added to all three by hand.
3. **Generated headers are shared.** CMake writes `lj_*def.h`, `luajit.h`
   and `jit/vmdef.lua` into `src/`, the same paths the Makefile writes. Two
   build directories, or a Make build and a CMake build, overwrite each other.
   Because the sources use quoted includes, a stale copy in `src/` always takes
   precedence over a copy in a build directory.
4. **Native MSVC is not exercised by CI.** Windows CI cross-compiles with
   MinGW and runs under Wine. The MSVC-specific fixes on this branch
   (`db5004035`, `c55a6ddec`, `7c8b7b0c6`, the ARM64 fix) are validated only
   on a developer machine.
5. **Local MSVC builds rely on configuration outside the repository.**
   `build-msvcrt/build.ninja` links `synchronization.lib` and
   `win/stdio_legacy.lib`, and the cache points at a different path
   (`C:\Development\...`) than the checkout. A fresh configure of the
   committed `CMakeLists.txt` would not reproduce that build.
6. **The test harness assumes Make.** `tests/lib/ljtest.lua` shells out to
   `make -C src`, so suites cannot run against a CMake or Ninja build without
   changes.

## Frustrations to collect

The maintainer has noted that there are several frustrations with the current
builds. Record each one here during the discussion, with the platform and
toolchain where it occurs:

- _(to fill in)_

## Options

These are starting points for the discussion, not recommendations to execute.

### A. Make is canonical; remove the other two

Keep `src/Makefile` as the only build. Restore a native MSVC route either as an
`msvcbuild.bat` equivalent or as `make` with an MSVC toolchain wrapper.

- For: matches upstream LuaJIT, CI and the test harness already use it, and
  it is the only system that derives DynASM flags correctly.
- Against: native MSVC on Windows is awkward without a batch file; IDE
  integration (`compile_commands.json`) needs `bear` or similar.

### B. CMake is canonical; Make kept only as the upstream-compatible path

Make CMake portable (Linux/macOS/Windows, MSVC/GCC/Clang). Derive DynASM
flags from `lj_arch.h` with a configure-time preprocessor probe. Generate
into the build directory. Teach `tests/lib` to drive CMake. Remove
`configure.lua`.

- For: one cross-platform system with native MSVC, IDE support and
  `install()`/pkg-config handling.
- Against: largest one-time effort; the Makefile still exists for upstream
  merges, so two systems remain unless the Makefile is also retired.

### C. `configure.lua` + Ninja is canonical

Bootstraps with `minilua`, so it needs no external configure tool, only Ninja.

- For: no CMake dependency; written for this tree.
- Against: least tested, not used by CI or tests, and it re-implements
  toolchain probing that CMake and Make already do.

### D. Make for CI and releases, CMake only for MSVC development

Keep both, but generate the shared data (source lists, DynASM flags) from one
place, and keep generated files out of `src/` for CMake.

- For: smallest change from today.
- Against: still two systems to maintain and keep in step.

## Questions to settle

1. Which system is canonical for CI and release artifacts?
2. Must native MSVC builds be supported for the release, or is MinGW enough
   for Windows? If MSVC is required, does it need CI coverage?
3. Must the build stay mergeable with upstream LuaJIT's Makefile changes?
4. Should generated headers move out of `src/` for every build system? If so,
   how should existing stale copies in `src/` be detected or removed?
5. Should the custom MSVC runtime setup in `win/` (`msvcrt.lib`,
   `stdio_legacy`, `dll_main.c`) be part of the supported build, or stay a local
   experiment?
6. What happens to `tools/build/` (including `minitest.lua` and the
   `ninja_syntax` port) if it is not chosen?
7. Should the test harness be build-system-agnostic (for example, take a
   prebuilt `luajit` path) rather than invoking a build itself?

## Decision record

_(Fill in once the discussion concludes: chosen option, date, and which items
in [17_build_issues.md](17_build_issues.md) become obsolete.)_
