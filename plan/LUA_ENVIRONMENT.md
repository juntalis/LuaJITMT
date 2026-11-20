# LuaJIT-MT: Lua Environment Reference

Quick reference for what Lua language/stdlib features this fork actually supports. Written for future prompts/agents that need to know the ground truth before writing or reviewing Lua/C code here.

## Identity

- Base: LuaJIT 2.1, Lua 5.1 grammar (`LUA_VERSION_NUM 501` in `src/lua.h`).
- Plus: moonjit-derived library/API extensions (commit `471caefec`).
- Plus: this fork's own headline feature — a lockless, shared-heap, multi-OS-thread runtime (`threading.*`), still under active implementation. See `plan/00_README_AGENT.md` and `plan/15_lockless_completion.md` for current status; earlier numbered plan docs (01-14) are architecture reference, **not** proof of what's built.

## Language grammar — what's NOT there

Don't write or expect:
- `<const>` / `<close>` attributes (5.4) — absent.
- Native bitwise operators `& | ~ << >>` or floor-div `//` as syntax (5.3/5.4) — use `bit.*` library instead.
- A true 64-bit integer subtype — `math.maxinteger`/`mininteger` are double-backed at ±2^53, not real integers. Anything relying on wraparound int64 semantics will not behave like real Lua 5.3/5.4.

## Language grammar — what IS there

- Standard Lua 5.1 + stock LuaJIT 2.1 extensions: `goto`/`::label::`, hex float literals, `\u{XXXX}` string escapes.

## Standard library — deltas from stock LuaJIT 2.1

Everything not listed below behaves like stock LuaJIT 2.1 (`ffi`, `bit`, `debug`, `package`, `io`, base lib, `jit.*`).

| Module | Addition | Note |
|---|---|---|
| `string` | `string.pack` / `string.packsize` / `string.unpack` | Lua 5.3-style format strings (`b B h H l L j J T i[n] I[n] f d n c[n] s[n] z x X < > = !n`) |
| `string` | NUL-safe pattern matching | Patterns work correctly over embedded `\0` bytes |
| `math` | `math.tointeger`, `math.type`, `math.ult`, `math.maxinteger`, `math.mininteger` | Double-backed exact-integer range (±2^53), **not** a real int64 subtype — don't assume wraparound |
| `os` | `os.date` calls `tzset()` before `localtime_r` | `TZ` env var changes are now honored per-call |
| `utf8` | New module: `char, charpattern, codepoint, codes, len, offset` | Strict scalar validation (rejects overlong encodings, > U+10FFFF, surrogates) — eagerly loaded as a global (not lazy-`require`d) |
| `jit.util` | `funcbc` takes optional 3rd `lineinfo` arg → returns `(ins, mode, line)` | Opt-in, default 2-arg behavior unchanged |
| `jit/bc.lua`, `bcsave.lua`, `dump.lua` | New `-L` / `-k` flags, `bL` dump mode | Source-line column + KGC/KN constant dump in bytecode listings |
| C API | `lua_absindex`, `lua_rawlen`, `lua_len`, `lua_setexdata`/`lua_getexdata`, `luaL_len`, `luaL_getsubtable`, `luaL_tolstring`, `luaL_pushresultsize`, `luaL_requiref`, `luaJIT_compat52` | Lua 5.2-shaped C API additions |
| `buffer` | Byte-buffer module: `new, put, putf, get, putcdata, reserve, commit, ref, encode, decode` | Pre-existing in this fork, not from the moonjit merge |

## `threading.*` — the multithreading API (this fork's core feature)

Lua-visible surface (`src/lib_threading.c`):
- `threading.spawn(fn, ...)` → joinable thread handle (real OS thread + new `lua_State` sharing the heap)
- `threading.current()` → handle for calling thread
- Thread handle: `:join()`, `:id()`, `:running()`
- `threading.mutex()` → `:lock()`, `:trylock()`, `:unlock()`
- `threading.channel(n)` → `:send()`, `:recv()`, `:peek()`, `:close()`
- `threading.cpucount()`, `threading.now()`, `threading.fence()`, `threading.sleep(sec)`
- `threading.gcstats()`, `threading.gcworkers([n])`, `threading.gcmode([mode])`
- `threading.exdata([ptr])` — opaque per-state `void*`, FFI cdata; deliberately non-foldable/non-CSE'd in JIT-compiled code so cross-thread updates stay visible. Also preloaded as `thread.exdata` (must be `rawequal` to `threading.exdata`).
- `utf8` loads eagerly (`lj_lib_load[]`); `threading`, `ffi`, `thread.exdata` are lazy (`lj_lib_preload[]`, i.e. `require`d).

### Nuanced/load-bearing behavior to know before touching concurrency code

- **No locks on hot paths.** Interpreter, JIT, and allocator never take mutexes/condvars. pthread primitives are allowed only for thread parking (futex wait in channel recv/join/GC-worker idle), one-time init, and the panic path.
- **All cross-thread-shared state must go through `lj_atomic.h`.** Raw/volatile shared loads or stores are a bug by policy; `src/lj_mtfields.md` is the living data dictionary of every shared field and its required ordering (load-acquire/store-release, CAS, RCU-pointer, etc.) — consult it before adding or auditing a shared field.
- **GC is concurrent, on-the-fly, non-stop-the-world** (mark/sweep in `lj_gc2.*`, arena allocator in `lj_arena.*`, replacing `lj_gc.*`/`lj_alloc.*` which are retained for reference/compat). Passing a functional/smoke test is **not** evidence of lock-free correctness or memory safety for this code — the project CLAUDE.md requires concurrency-appropriate evidence.
- **Bytecode dump version 4** ("lockless" format) adds cell-upvalue opcodes for shared mutable upvalues across threads. Old v2/v3 dumps still load if they avoid lockless-only opcodes, but cannot be re-dumped as v4.
- **State ownership is CAS-protected**: a spawned `lua_State` is handed to a new OS thread via `lj_state_claim`/`lj_state_owner_word_cas`; don't assume a `lua_State*` is single-owner without checking this.
- **Supported platforms**: x86-64 only, Linux/macOS/Windows, `LJ_GC64` mandatory. Linux is the implementation-first target (per a 2026-09-04 directive); macOS (Darling) and Windows (Wine) validation is deferred to pre-release. Don't assume 32-bit or non-x86-64 support.
- Trace tokens from JIT-compiled code are cancelled on worker-thread teardown (`lj_trace_abort_owner`) — traces don't outlive the thread that owns them.

## Where to look for more

- `plan/00_README_AGENT.md` — entry point, current execution plan, binding design rules.
- `plan/15_lockless_completion.md` — current authoritative implementation status (supersedes 01-14 on status claims).
- `plan/MOONJIT_EXTRACTION_IMPLEMENTATION_PLAN.md` — scope/intent of the moonjit library extraction (a plan, not proof of completion).
- `src/lj_mtfields.md` — per-field atomicity/ordering dictionary for shared state.
