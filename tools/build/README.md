# LuaJIT Ninja Build Generator

`configure.lua` generates a Ninja build (`tools.ninja`, `config.ninja`,
`build.ninja`) for this LuaJIT-MT tree without relying on GNU Make, NMake, or
any other `make` variant. It reimplements the same two-stage bootstrap as
`src/Makefile` (minilua -> DynASM -> buildvm -> generated headers -> core
sources -> library -> `luajit` executable) as plain Ninja build edges driven
by whichever compiler toolkit you select.

It is a plain Lua script with no external dependencies beyond the Lua
standard library subset available to `minilua` (see
`../../notes/mini-lua-interpreter-surface.md`), and can be run with any Lua
5.1-compatible interpreter (`lua`, `luajit`, or a previously built
`minilua`).

## Requirements

- A Lua 5.1-compatible interpreter to run `configure.lua` itself.
- [Ninja](https://ninja-build.org/) to run the generated build.
- One of the supported compiler toolkits on `PATH` (or reachable via `CC`):
  - `msvc` -- `cl.exe`/`link.exe`/`lib.exe` (run from a `vcvarsall.bat`-
    initialized shell).
  - `gcc` -- `gcc`/`ar`, or any GCC-compatible toolkit including MinGW and
    Clang (set `CC`/`LD`/`AR` to override).

This fork only supports the x64 architecture (see `src/lj_arch.h`), so
`configure.lua` targets x64 on Windows, Linux, or macOS.

## Usage

```
lua configure.lua [OPTION]... [LUAJIT_SRC_DIR]|-
```

`LUAJIT_SRC_DIR` is the path to the LuaJIT `src/` directory (default `.`,
i.e. run `configure.lua` from inside `src/`). All generated `.ninja` files
are written to the current directory.

### Options

| Option | Values | Default | Description |
| --- | --- | --- | --- |
| `-h`, `--help` | | | Display help and exit. |
| `-V`, `--version` | | | Display version info and exit. |
| `-t`, `--toolkit` | `msvc`, `gcc` | *autodetected, or required* | Compiler toolkit to generate rules for. |
| `-c`, `--config` | `release`, `debug` | `debug` | Optimization/debug-info level. |
| `-o`, `--type` | `static`, `shared` | `shared` | Static library or shared library build (see below). |
| `--target-os` | `windows`, `linux`, `macos` | *autodetected, or required* | Target OS. |
| `--build-dir` | path | `build` | Directory for object files, libraries, and executables. |
| `--install-dir` | path | *(none)* | When set, adds an `install` target that stages the executable and library. |
| `--with-tests` | | off | Reserved: parsed but not yet wired into the build graph. |
| `--with-bench` | | off | Reserved: parsed but not yet wired into the build graph. |
| `--without-ffi` | | *(FFI enabled)* | Disable the FFI library (`LUAJIT_DISABLE_FFI`). |
| `-v`, `--verbose` | | | Print full commands instead of short step descriptions. Repeatable. |
| `-q`, `--quiet` | | | Suppress per-step descriptions (opposite of `-v`). |

`--toolkit` has no hardcoded default: it's inferred from the environment
(`VCINSTALLDIR`/`VSCMD_ARG_TGT_ARCH`/`VisualStudioVersion` implies `msvc`;
otherwise a `CC` environment variable implies `msvc` or `gcc`) and must be
given explicitly with `-t`/`--toolkit` if neither is set. `--target-os` is
inferred the same way (`msvc` implies `windows`; otherwise the `OS`
environment variable or the host's path-separator convention is used) and
falls back to requiring `--target-os` explicitly.

`CC`, `LD`, `AR`, `CFLAGS`, `LDFLAGS`, and `ARFLAGS` are read from the
environment where relevant and folded into the generated `tools.ninja`.

### `--type static` vs `--type shared`

Only one of the two is produced per `configure.lua` run (this generator does
not implement `src/Makefile`'s POSIX "mixed" default). Output naming:

| | Windows | Linux | macOS |
| --- | --- | --- | --- |
| `shared` | `luajit.exe`, `lua51.dll`, `lua51.lib` (msvc) / `liblua51.dll.a` (mingw) | `luajit`, `libluajit-5.1.so` | `luajit`, `libluajit-5.1.dylib` |
| `static` | `luajit.exe`, `liblua51.lib` (msvc) / `liblua51.a` (mingw) | `luajit`, `libluajit-5.1.a` | `luajit`, `libluajit-5.1.a` |

## Examples

Generate and run a Release build with MSVC/x64 and Ninja, from a
`vcvarsall.bat x64`-initialized shell, at the repo root:

```
lua tools\build\configure.lua -t msvc -c release --build-dir out src
ninja
out\luajit.exe -v
```

Generate a static Debug build with GCC on Linux:

```
lua tools/build/configure.lua -t gcc -c debug -o static --build-dir build src
ninja
./build/luajit -v
```

Auto-detect the toolkit/OS (works when `CC` or MSVC environment variables
are already set) and stage an install tree:

```
lua tools/build/configure.lua --install-dir /usr/local src
ninja install
```

## `ninja.lua` / `ninja_syntax.lua`

`configure.lua` is built on two supporting modules in this directory:

- `ninja_syntax.lua` -- a Lua port of Ninja's
  [`ninja_syntax.py`](https://github.com/ninja-build/ninja/blob/master/misc/ninja_syntax.py),
  used to emit correctly-escaped `.ninja` files (`ninja.Writer`).
- `ninja.lua` -- a small command-line argument parser built on top of
  `ninja_syntax.lua`, used by `configure.lua` to parse its options into a
  single `opts` table.

### Tests

```bash
lua ninja_syntax_test.lua
```
