# MoonJIT extraction implementation plan

## Purpose and scope

This file is the handoff for a new implementation session. It describes how to
extract every High- and Medium-priority MoonJIT candidate selected during the
branch review, while preserving LuaJITMT's lockless shared-heap design. It is a
plan, not evidence that any item has already been implemented.

Implement these candidates:

1. Lua 5.2 C API additions and the `luaJIT_compat52` ABI marker.
2. Optional bytecode source-line and constant-table inspection.
3. POSIX timezone refresh for `os.date`.
4. Binary-safe string patterns containing embedded NUL bytes.
5. Per-`lua_State` external data (`exdata`), including C, Lua, and JIT access.
6. The Lua 5.3 `utf8` library.
7. Lua 5.3 `string.pack`, `string.packsize`, and `string.unpack`.
8. Lua 5.3-style integer-limit and integer-classification functions in `math`.

Do **not** implement the Low-priority candidates in this effort: trace-ring
logging, `jit.prngstate`, or hardware-CRC string hashing. Do not broaden the
work into CI, documentation, packaging, unsupported CPU ports, or unrelated
MoonJIT fixes.

The MoonJIT branch is an implementation reference, not the merge target. Its
tip is `a2a39ea7` and the merge base found during analysis was
`f0e865dd4861520258299d0f2a56491bd9d602e1`. Inspect individual commits with
`git show <commit>` and files with `git show moonjit-master:<path>`. Do not
cherry-pick whole commits: MoonJIT is based on an older LuaJIT and lacks this
fork's state claims, atomic publication, GC rooting, generated-header flow, and
current JIT/GC invariants.

## Mandatory orientation for a fresh session

Before editing:

1. Read this file completely.
2. Read `plan/00_README_AGENT.md` and `plan/15_lockless_completion.md`.
3. Run `git status --short` and preserve all pre-existing user changes.
4. Confirm that `moonjit-master` and the current working branch exist with
   `git branch --all` and record the current commit.
5. Run `src/luajit tools/test.lua --list` (or build first if the binary is
   absent) to learn the current suite names.
6. Establish a clean baseline using the default build and the stock tests.

The non-negotiable project rules are:

- The supported targets are x86-64 Linux, macOS, and Windows with mandatory
  GC64. Linux is the first implementation target, but completion includes the
  other supported release targets.
- No hot-path locks may be introduced.
- Every field that can be observed across native threads must be accessed via
  `src/lj_atomic.h` or an existing typed atomic accessor.
- Do not insert a safepoint inside a sequence whose correctness depends on
  ownership, rooting, pinning, or publication remaining unchanged.
- Public API code in `src/lj_api.c` must use the current `LJStateClaim`,
  resume-claim, snapshot, stack-root, stack-growth, and publication patterns.
  Never paste old MoonJIT API bodies that read `L->top`, stack slots, tables, or
  GC objects without the required current protection.
- Do not retain an unrooted GC pointer or a stack pointer across an allocation,
  callback, metamethod, stack growth, or other operation that can collect or
  relocate state. Reacquire after the operation when needed.
- Tests must verify public behavior or emitted artifacts. They must not pass by
  grepping the implementation source.

## Test organization and delivery strategy

Create `tests/suites/compat_extensions.lua` and register it in
`tests/suites/init.lua`. Use the existing helpers in `tests/lib` for builds,
temporary files, C fixture compilation, process execution, stock tests, and
platform detection. Provide focused cases with these stable names:

- `compat_api52`
- `compat_bcdump`
- `compat_os_date_tz`
- `compat_pattern_nul`
- `compat_exdata`
- `compat_stdlib53`
- `compat_extensions`, an aggregate depending on all cases above

Keep each focused case independently runnable. Where a feature has interpreter
and recorder behavior, test both `-joff` and `-jon`. Build-profile changes must
use the suite's serialized build helpers and restore the default build. Put C
fixtures under `tests/fixtures/` with descriptive `compat-*` names.

Implement in the milestones below. Build and run the focused case after every
milestone; make reviewable commits at milestone boundaries. Do not defer all
tests until the end.

## Milestone 0: baseline and contract lock

Record the outputs of:

```sh
make -C src -j
src/luajit -e "require'ffi'; print(2^31)"
src/luajit tools/test.lua --list
tools/ci/lua_test.sh run_stock_tests
```

On a platform where `make -j` is unavailable, use the repository's existing
native or cross-build route. Do not invent a new build system as part of this
work.

Before implementation, turn every behavior described below into a test or a
short contract comment in the corresponding focused case. This avoids silently
copying MoonJIT quirks where this plan intentionally chooses safer behavior.

## Milestone 1: Lua 5.2 C API and compatibility marker

### Reference commits

Use these MoonJIT commits only as behavioral and naming references:

`cbfaa201a`, `d3bdc1e65`, `c743f0891`, `c0d130b4c`, `55afeb474`,
`fc23b2c19`, `4d883f90c`, `291ffb114`, `249ca9171`, `ffe1c65eb`,
`b38809e77`, and `d28551098`.

### Public declarations

Edit `src/lua.h` and add these exported core API declarations in the existing
Lua 5.2 API section:

```c
LUA_API int lua_absindex(lua_State *L, int idx);
LUA_API size_t lua_rawlen(lua_State *L, int idx);
LUA_API void lua_len(lua_State *L, int idx);
LUA_API void lua_setexdata(lua_State *L, void *data); /* implemented in M4 */
LUA_API void *lua_getexdata(lua_State *L);            /* implemented in M4 */
```

The exdata declarations may land in Milestone 4 instead; they are listed here
to keep ownership of the public header clear. Change the compatibility macro
from `lua_strlen(L,i) lua_objlen(L,(i))` to
`lua_strlen(L,i) lua_rawlen(L,(i))`. Retain `lua_objlen` as an exported legacy
API; do not make it an alias for `lua_rawlen` because its number-coercion
behavior is different.

Add these declarations to `src/lauxlib.h` near their related helpers:

```c
LUALIB_API lua_Integer luaL_len(lua_State *L, int idx);
LUALIB_API int luaL_getsubtable(lua_State *L, int idx, const char *fname);
LUALIB_API const char *luaL_tolstring(lua_State *L, int idx, size_t *len);
LUALIB_API void luaL_pushresultsize(luaL_Buffer *B, size_t sz);
LUALIB_API void luaL_requiref(lua_State *L, const char *modname,
                             lua_CFunction openf, int glb);
```

Add `lua_pushglobaltable(L)` as the standard macro that pushes
`LUA_GLOBALSINDEX`. Put auxiliary APIs in `lauxlib.h`, not `lua.h`, even if an
old MoonJIT patch placed one there.

`src/luajit.h` is generated and normally absent from the worktree. Add
`LUA_API int luaJIT_compat52;` to `src/luajit_rolling.h`; the target around
`src/Makefile:661` generates the installable header. Never hand-edit or commit
the generated `src/luajit.h`. Define the symbol in `src/lj_api.c` as an exported
`int` initialized to `LJ_52`. Keep the ABI type exactly `int`; the value is
immutable by convention after process initialization, even though `const`
cannot be added without changing the MoonJIT-compatible ABI. Verify that the
generated public header contains the declaration and that shared-library builds
export the symbol.

### Core API semantics and MT implementation rules

Implement the core functions in `src/lj_api.c` using adjacent current APIs as
templates:

- `lua_absindex`: return `idx` unchanged when `idx > 0` or
  `idx <= LUA_REGISTRYINDEX`; otherwise return `lua_gettop(L) + idx + 1`.
  Calling the public `lua_gettop` is preferable to an unclaimed raw read.
- `lua_rawlen`: strings return byte length, tables return the raw sequence
  length through `lj_tab_len_rooted`, userdata returns payload size, and all
  other types return zero. It must not convert numbers to strings and must not
  invoke `__len`. Use the current read snapshot and root patterns. Root table
  operands across a length operation whenever the current helper requires it.
- `lua_len`: push exactly one result. Strings push their byte length. Tables use
  a `__len` metamethod when the `LJ_52` build mode enables that behavior and the
  metamethod exists; otherwise they push raw table length. Userdata and other
  applicable values follow the current VM's `BC_LEN`/`lj_meta_len` behavior.
  Preserve the metamethod result without coercion. Acquire a resume claim,
  reserve stack space, root the operand across a metamethod call, publish the
  pushed result, and release the claim on every normal exit. Use current
  `lua_concat`, comparison APIs, and the interpreter length opcode as patterns.

Do not call `lua_objlen` to implement either new function.

### Auxiliary API semantics

Implement these in `src/lib_aux.c`, predominantly through public C APIs so they
inherit the correct claim/root behavior:

- `luaL_len` calls `lua_len`, converts the result with `lua_tointegerx`, removes
  the temporary result on success, and errors with
  `object length is not a number` when conversion fails. This deliberately
  follows MoonJIT's final API patch, including LuaJIT's normal numeric-string
  conversion, rather than imposing a stricter integral-result check.
- `luaL_getsubtable` converts `idx` to an absolute index before any stack
  mutation. It leaves the existing table on top and returns 1. If the field is
  absent or not a table, it replaces the fetched value with a new table, stores
  that table in `fname`, leaves it on top, and returns 0.
- `luaL_tolstring` honors `__tostring`. Without it, strings and numbers are
  converted using normal Lua conversion; booleans and nil use their literals;
  other values use `typename: pointer`. The final value remains rooted on the
  stack and is passed through `lua_tolstring`, with its byte length returned
  through `len` when non-NULL. Thus a numeric metamethod result is converted to
  a string, while a non-convertible result leaves that result on the stack and
  returns NULL, matching MoonJIT's Lua 5.2 backport.
- `luaL_pushresultsize` performs the standard `luaL_addsize(B, sz)` followed by
  `luaL_pushresult(B)`. Add an API assertion that `sz` fits the currently
  prepared buffer when that can be expressed without breaking the established
  buffer contract.
- `luaL_requiref` uses registry table `_LOADED`, not the mutable global
  `package.loaded`. If no truthy cached value exists, call `openf(modname)`,
  cache its single result, and leave the module value on the stack. If `glb` is
  nonzero, also assign the same value to the named global. Preserve the module
  value on top in both cached and newly opened paths.

`src/lib_aux.c` currently has a private `abs_index` helper/macro. Remove it or
replace its users with `lua_absindex` so there is one definition of the index
contract.

### Milestone 1 tests

Add a compiled external C fixture. It must include only installed/public
headers and link against the produced library. Verify:

- symbol declarations and successful linkage;
- stack deltas and result types for every API;
- positive, negative, registry, and globals pseudo-indices for `lua_absindex`;
- raw versus metamethod table length, userdata length, string byte length,
  number behavior, and `lua_objlen` remaining backward compatible;
- `luaL_tolstring` for all type groups, embedded NUL strings, string and numeric
  `__tostring` results, and the specified NULL result for a non-convertible
  `__tostring` result;
- `luaL_getsubtable` existing, missing, and non-table fields;
- `luaL_requiref` calls the opener once, survives replacement/removal of the
  global `package` table, caches in registry `_LOADED`, honors `glb`, and leaves
  one result;
- buffer completion with `luaL_pushresultsize`;
- `luaJIT_compat52 == 0` in the default build and `== 1` in a serialized build
  using `-DLUAJIT_ENABLE_LUA52COMPAT`.

Run the fixture against static and shared builds where the platform supports
both. Inspect dynamic exports with `nm -D`, `otool`/`nm`, or `dumpbin` as
appropriate; checking only that an in-tree static link succeeds is not enough
for the ABI marker.

## Milestone 2: optional bytecode inspection

### Reference commits

- `58e9941b6`: line information in bytecode listings.
- `cce112ca4`: KGC/KN constant-table display.
- `bd304a366`: source locations following recorded bytecode in `jit.dump`.

Default output is compatibility-sensitive. All new output must be opt-in.

### Pin the public Lua contracts

Extend `jit.util.funcbc(func, pc [, lineinfo])`. Existing two-argument calls
still return `(instruction, mode)`. When the optional third argument is truthy,
return `(instruction, mode, line)`, where line is `lj_debug_line(pt, pc)` and is
zero when debug line data is stripped or unavailable.

Extend the Lua bytecode module without breaking existing callers:

```lua
jit.bc.line(func, pc, prefix [, lineinfo])
jit.bc.dump(func [, out [, all [, options]]])
```

For `jit.bc.dump`, a fourth argument of `true` is the Moon-compatible shorthand
for `{ lineinfo = true }`; otherwise it may be a table containing
`lineinfo = true` and/or `constants = true`. Existing first three arguments and
their defaults remain unchanged.

Extend `luajit -b` options as follows:

- `-l` remains the current instruction-only listing.
- `-L` implies a listing and adds a source-line column.
- `-k` implies a listing and appends KGC and KN constant tables.
- Combined spellings such as `-bLk` enable both additions.

Display a known line as `[123]` and unavailable/stripped line information as
`[-]`. Constant output must be deterministic and must not contain heap pointer
addresses. Use records of the form `KGC <index> <value>` and
`KN <index> <value>`. Escape string bytes consistently with the listing's
existing constant formatter; represent child prototypes by source/location,
tables as `table`, and unsupported GC constant kinds by a stable type label.
Format numeric constants through one shared deterministic formatter. Reuse the
same value-formatting path for instruction comments and table dumps to avoid
two subtly different escaping rules.

Add `L` as an opt-in `jit.dump` mode modifier. With bytecode dumping enabled,
mode `bL` appends the current `fmtfunc(func, pc)` source location after each
recorded instruction. `L` alone does not enable `b`. Preserve HTML escaping
when HTML output is selected. Existing `b` output must remain byte-for-byte
unchanged when `L` is absent.

### Files and implementation notes

- `src/lib_jit.c`: extend `jit.util.funcbc` while the function/prototype is
  rooted according to the current code. Prototype debug arrays are immutable,
  but that does not waive rooting and lifetime rules.
- `src/jit/bc.lua`: carry the line/constant options through `line` and `dump`.
- `src/jit/bcsave.lua`: parse `L` and `k`, make either imply listing, and pass
  the options table to `jit.bc`.
- `src/jit/dump.lua`: recognize the `L` modifier and append a source location
  only on the `b` output path.

### Milestone 2 tests

Generate a temporary Lua source file with several source lines, a nested
function, a table constant, string constants containing control bytes, and
numeric constants. Invoke the public CLI and modules and assert:

- default `-bl` has neither the new line column nor KGC/KN sections;
- `-bL` includes expected source lines;
- `-bk` includes KGC and KN sections;
- `-bLk` includes both and output is identical across two runs;
- stripped bytecode displays `[-]` rather than reading invalid debug data;
- old `jit.util.funcbc` callers still receive two useful return values;
- the optional form returns a third line value;
- `jit.dump` mode `b` remains unchanged, while `bL` includes source locations;
- text and HTML modes escape all appended data correctly.

Compare output artifacts, not source text.

## Milestone 3: timezone refresh and binary-safe patterns

### POSIX timezone refresh

Reference commit: `372bb8b22`.

In `src/lib_os.c`, call `tzset()` immediately before the non-UTC
`localtime_r` path in `os_date_tm`. Do not call it for the UTC (`!`) path and do
not alter Windows behavior. Add a concise comment: POSIX does not require
`localtime_r` to refresh process timezone state after `TZ` changes, so `tzset`
does that explicitly. This is a C-library/native-global boundary; it does not
justify adding a VM lock. The supported contract is that callers externally
synchronize concurrent mutation of the process `TZ` environment.

Test this in an isolated POSIX C fixture because ordinary Lua does not expose a
portable `setenv`: set `TZ=UTC0`, call `os.date` at a fixed timestamp, set
`TZ=EST5`, call it again in the same Lua state, and verify the expected five
hour change. Choose a timestamp/form that cannot cross into ambiguous DST. The
fixture must restore the previous environment if it runs in the test process;
prefer a child process. Skip with an explicit reason on Windows. Concurrent
`os.date` calls may be stressed, but do not test or promise unsynchronized
concurrent `TZ` mutation.

### Embedded NUL bytes in patterns

Reference commits: `7d6c15659`, `3e5cd6670`, `19d29ab74`, and the final
anchored-`gsub` correction `cabe3b3cb`.

The current matcher in `src/lib_string.c` uses C NUL as its pattern terminator.
Make it fully length-bounded:

1. Add `p_end` to `MatchState` and initialize it from the pattern `GCstr` byte
   length in `find`/`match`, `gmatch`, and `gsub` paths.
2. At the start of `match`, success means `p == p_end`; a byte with value zero
   before `p_end` is an ordinary literal pattern byte.
3. Audit **every** pattern read and lookahead. `classend`, escapes, bracket
   classes, `%b`'s two required bytes, `%f`, capture references, `$` as the
   final pattern byte, and quantifier detection must check against `p_end`.
4. Compute original `pattern_start` and `pattern_end` once. If the first byte is
   `^`, advance only the start. Never recompute the end from the shortened
   start plus the original length; that was the source of MoonJIT's later
   anchored-`gsub` fix.
5. Keep plain search on the existing length-aware `lj_str_find` path and retain
   `%z`/`%Z` compatibility.

Malformed truncated constructs must raise the same class of pattern error as
their textual equivalents, never read beyond the string, hang, or silently
match.

Test literal NUL at the start/middle/end; NUL adjacent to magic characters; a
bracket class containing NUL; captures; `%b` missing either required byte;
frontiers; capture references; `$` following an embedded NUL; empty matches;
`gmatch` progress; and anchored and unanchored `gsub`, including a direct
regression for `cabe3b3cb`. Add malformed truncated escape and bracket cases.
Run the same behavioral fixture with JIT off and on.

## Milestone 4: external per-state data (`exdata`)

### Reference commits and chosen contract

Reference `4abab7642`, `13f03a44f`, `74a5510fb`, `da70b450c`, and
`1e5a1c2a3`.

Each `lua_State` owns one opaque `void *` value:

- Main states initialize it to NULL.
- A newly created coroutine receives one snapshot of its parent's value before
  publication.
- Parent and child values are independent after creation; later writes do not
  propagate automatically.
- `lua_setexdata` stores with release semantics and `lua_getexdata` loads with
  acquire semantics. Neither API claims the Lua state or allocates. They are
  wait-free pointer operations intended for host TLS-style metadata access.
- LuaJITMT never traces, dereferences, retains, or frees the pointer. Its
  pointee lifetime and synchronization are entirely the embedding
  application's responsibility.

### State layout and lifecycle

Add a naturally aligned `void *exdata` field to `lua_State` in `src/lj_obj.h`.
Do not read or write the field directly outside initialization helpers. Add
typed inline accessors near the existing `lua_State` atomic accessors, for
example `lj_state_exdata_acq`, `lj_state_exdata_rel`, and an initialization-only
relaxed store. Implement them with `la_loadptr_acq`/`la_storeptr_rel` and the
project's pointer-cast conventions. Document the acquire/release publication
contract at the accessors.

Initialize the main state's field to NULL before the state is published. In
`state_new_withenv_at_anchor` (or its exact current construction helper), load
the parent's exdata once with acquire semantics and store it into the child
before the child object's READY/publication sequence. Audit all alternate state
construction and restore paths so no state reaches publication uninitialized.
No GC marking/freeing change is needed because the pointer is opaque.

Implement and export the C APIs in `src/lj_api.c`; add declarations in
`src/lua.h`. Verify symbol visibility in shared builds on every supported
platform.

### Lua surface

When `LJ_HASFFI` is enabled:

- Add `threading.exdata([ptr_or_nil])` to the existing `threading` module.
- With no argument, return a `void *` cdata containing the acquire-loaded
  pointer.
- With one argument, accept pointer-compatible cdata or nil, release-store the
  pointer (nil clears it), and return no values. Reject numbers, strings, and
  incompatible cdata with a normal argument error.
- Preload `thread.exdata` as a compatibility module whose required value is the
  exact function object stored at `require("threading").exdata` (not merely a
  second closure over the same C function). Its loader can require/fetch the
  existing module through public APIs. Add an opener only if needed by
  `src/lib_init.c` and declare it in `src/lualib.h` when it crosses compilation
  units.

The C API remains available when FFI is disabled. In that build the Lua-level
entry points must be absent cleanly; do not leave a loader that fails halfway.

### Recorder correctness

Do **not** port MoonJIT's ordinary `IR_FLOAD` of `lua_State.exdata`. Another
native thread may update the field, and an ordinary field load can be commoned,
folded, or hoisted, producing stale values.

Record the getter as a non-foldable helper call returning an IR pointer. Add an
IR-call descriptor in `src/lj_ircall.h` with flags that model an externally
observable read and prevent CSE/hoisting. The helper must perform the same
acquire load as the C API. Convert the returned pointer to the standard `void *`
cdata result using current recorder conventions. If the IR call machinery
cannot express an acquire external read without optimizer ambiguity, use the
existing volatile-load mechanism instead and document why.

The setter should abort recording/NYI and execute through the interpreter on
trace exit. This keeps cross-thread publication explicit and avoids adding a
side-effecting store IR solely for this feature. Register the getter recorder
in `src/lj_ffrecord.c` and ensure cdata allocation/sinking follows current GC64
patterns.

### Milestone 4 tests

Use an external C fixture plus Lua fixtures to verify:

- initial NULL, set/get, clear, and no pointer tearing;
- child inheritance at creation and independent subsequent parent/child writes;
- inheritance for `lua_newthread` and for states used by `threading.spawn`;
- coroutine migration/resumption does not change the state-local value;
- repeated remote release stores and acquire loads observe only complete pointer
  values under stress;
- Lua getter/setter behavior under `-joff` and `-jon`;
- `rawequal(require("threading").exdata, require("thread.exdata"))`;
- traced hot-loop reads can observe a remote update, proving the getter was not
  constant-folded, CSE'd, or hoisted;
- a setter encountered during recording exits safely and does not corrupt the
  trace;
- FFI-disabled builds retain working C symbols and omit Lua entry points;
- GC stress does not treat the pointer as a GC object.

Add a TSAN stress fixture following `m4_tsan_drivers`. Any race report on the
field is a release blocker. The test must not dereference arbitrary exdata
pointers.

## Milestone 5: Lua 5.3 `math` and binary packing

### Math subset

Reference commit: `b9ea9fbc4`.

Implement directly in `src/lib_math.c`, using normal C library entries rather
than an embedded generated Lua chunk. LuaJIT does not have the full Lua 5.3
integer subtype, so deliberately use MoonJIT-compatible exact-double limits:

```text
math.maxinteger =  9007199254740992  --  2^53
math.mininteger = -9007199254740992  -- -2^53
```

Pin behavior as follows:

- `math.tointeger(x)` accepts only values whose Lua type is number, are finite,
  integral, and in the inclusive range above. Return the numeric value or nil.
  Numeric strings are not accepted.
- `math.type(x)` returns nil for non-numbers, `"integer"` for values accepted by
  `math.tointeger`, and `"float"` for all other numbers, including infinities
  and NaN.
- `math.ult(m, n)` requires two integer-valued numbers in range and compares
  their conceptual unsigned ordering: any nonnegative value is below any
  negative value; values with the same sign use ordinary numeric comparison.
  Report the correct argument number for invalid inputs.

Avoid undefined behavior from converting NaN, infinity, or an out-of-range
double to an integer. Classify with numeric predicates and comparisons first.
No specialized recorder is required initially; a normal C function trace exit
is acceptable.

Test both boundaries, boundary-adjacent nonintegral and out-of-range values,
negative zero, NaN, infinities, numeric strings, booleans, argument errors, and
all four sign combinations for `math.ult` under JIT off/on.

### `string.pack`, `string.packsize`, and `string.unpack`

Reference `427bc1814` and refactor `c452c09bd`, but reimplement against the
current string library and buffer helpers.

Add `typedef size_t lua_Unsigned;` to `src/lua.h` next to `lua_Integer`. Do not
copy MoonJIT's `unsigned long`: it is only 32 bits on Win64 and breaks native
integer formats. The supported GC64 targets all use this pointer-sized unsigned
counterpart.

Register the three functions in `src/lib_string.c`. Support the Lua 5.3 format
set:

```text
b B h H l L j J T i[n] I[n]
f d n c[n] s[n] z x X
< > = !n and ASCII space
```

Use these implementation constraints:

- Native `j`/`J` use `sizeof(lua_Integer)`/`sizeof(lua_Unsigned)`; `T` uses
  `sizeof(size_t)`. Explicit integer sizes may be 1 through 16 bytes.
- Track unpack positions in a signed type until negative Lua indices have been
  normalized. Do not copy MoonJIT code that stores the position in `size_t` and
  then tests whether it is negative.
- Check every size addition, multiplication, alignment round-up, left shift,
  and stack result count before performing it. Reject format sizes or outputs
  that exceed `SIZE_MAX`, Lua string limits, stack capacity, or supported
  integer representation.
- Implement endian conversion bytewise or with defined unsigned operations;
  never shift a signed value or shift by its width.
- Enforce alignment for every data option, `!n`, and `X`. Reject invalid or
  non-power-of-two alignments according to Lua 5.3 behavior.
- `c[n]` consumes/emits exactly `n` bytes. `s[n]` prefixes a bounded-length
  string. `z` rejects strings containing NUL when packing and performs a
  bounded NUL search when unpacking.
- `string.packsize` errors for variable-length formats (`s` without a fixed
  representable prefix still has variable payload, and `z` is variable) and
  uses exactly the same format parser/alignment logic as pack/unpack.
- `string.unpack` returns all decoded values followed by the next 1-based byte
  position. It errors on truncated input and invalid starting positions.
- Floating formats preserve normal finite values, signed zero, infinities, and
  NaN payload behavior to the extent provided by the host representation. Use
  `memcpy`, not type-punning through incompatible pointers.
- Use the current per-thread-group temporary SBuf (`lj_buf_tmp_` and current
  `_tg` helpers). Root/publish the resulting string before `lj_gc_check`. Never
  retain an SBuf data pointer across a growth/allocation that can invalidate it.

Do not add recorder specializations in this milestone. Correct trace exits are
preferable to a broad, unreviewed binary-format recorder.

Tests must cover every format, both endian modes, native mode, alignment and
`X`, empty and embedded-NUL fixed strings, z-string rejection, signed and
unsigned limits at every size, overflow rejection, truncated input, negative
and boundary start positions, too many results, floats/doubles including NaN
and infinities, and round trips. Include a specific Win64 regression for
`j`/`J`, `T`, and `I8`. Run under JIT off/on and an assertion build; use
sanitizers where available.

## Milestone 6: Lua 5.3 `utf8` library

Reference commit: `17c55bcbd`.

Do not copy MoonJIT's generated `utf8.h` bytecode. Add a direct C implementation
as `src/lib_utf8.c`. This avoids stale bytecode-dump version coupling and makes
bounds and scalar validation reviewable.

Implement and export `luaopen_utf8` with:

- `utf8.char`
- `utf8.charpattern`
- `utf8.codepoint`
- `utf8.codes`
- `utf8.len`
- `utf8.offset`

Use strict Unicode scalar validation. Reject malformed/truncated continuation
sequences, overlong encodings, values above U+10FFFF, and surrogate code points
U+D800 through U+DFFF. `utf8.char` accepts only integral valid scalars.
Positions are byte positions and use Lua's 1-based/negative-index conventions.
Pin Lua 5.3 error behavior: `utf8.len` returns `nil, bad_byte_position` on an
invalid sequence; `codepoint` and the `codes` iterator raise an error;
`offset` raises when its initial position is a continuation byte. Export the
standard Lua 5.3 `charpattern` string.

Use shared decoder/position helpers so all entry points accept and reject the
same byte sequences. Decoder arithmetic must check bounds before every byte
read. The library has no shared mutable globals.

Integrate it everywhere a built-in library participates:

- add `lib_utf8.o` to `LJLIB_O` in `src/Makefile`;
- add the source/object dependencies in `src/Makefile.dep`;
- include `lib_utf8.c` in `src/ljamalg.c`;
- add `LUA_UTF8LIBNAME "utf8"` and `luaopen_utf8` to `src/lualib.h`;
- add it to the normal `lj_lib_load` array in `src/lib_init.c` so
  `luaL_openlibs` installs global `utf8`.

Do not hide the library behind `LJ_52`. The selected extension libraries and C
APIs are unconditional except for the Lua exdata surface's FFI dependency.

Test all scalar boundary encodings, ASCII, multibyte iteration, negative
positions, empty ranges, offset in both directions, `offset(s, 0, i)`, standard
`charpattern`, malformed lead/continuation bytes, every overlong width,
truncation at every byte, surrogate boundaries, U+10FFFF, and U+110000. Ensure
iterator errors report the bad byte position. Test normal and amalgamated
builds, static and shared library initialization, and JIT off/on.

## Milestone 7: integration, ABI, and release gates

### Required build/test matrix

At minimum run:

```sh
make -C src clean
make -C src -j
tools/ci/lua_test.sh compat_extensions
tools/ci/lua_test.sh run_stock_tests
tools/ci/lua_test.sh m0_matrix
tools/ci/lua_test.sh m0_lua52_compat
```

Also run an assertion build, a `LUAJIT_DISABLE_JIT` build, a
`LUAJIT_DISABLE_FFI` build, an amalgamated build, and the existing GC/TSAN
profiles that cover touched state and recorder paths. Use the repository's
build helper APIs so each profile is serialized and the default build is
restored even after a failure.

Run the existing release binary checks when their artifacts/runners are
available:

```sh
tools/ci/lua_test.sh release_linux_binary
tools/ci/lua_test.sh release_windows_binary
tools/ci/lua_test.sh release_macos_binary
```

Linux is not sufficient for sign-off. On Windows verify LLP64 packing semantics
and exports with the supported native toolchain. On macOS verify timezone,
shared-library exports, and amalgamation. If a runner is unavailable, record
the exact skipped gate and do not describe the overall work as cross-platform
complete.

### ABI and generated artifacts

Verify the public static and dynamic symbols for:

```text
lua_absindex
lua_rawlen
lua_len
lua_setexdata
lua_getexdata
luaL_len
luaL_getsubtable
luaL_tolstring
luaL_pushresultsize
luaL_requiref
luaJIT_compat52
luaopen_utf8
```

Build once from a clean checkout state to prove all generated files have proper
dependencies. Confirm `src/luajit.h` is regenerated from
`src/luajit_rolling.h`, contains `luaJIT_compat52`, and is not accidentally
tracked. Confirm `lib_utf8` is included in ordinary and amalgamated builds.

### Concurrency and GC audit

Before completion, review the final diff specifically for:

- every direct read/write of `lua_State.exdata` (only typed accessors and
  pre-publication initialization are allowed);
- any stack or GC pointer kept across allocation, callback, metamethod, or
  buffer growth;
- all new public API exits releasing claims and publishing new stack values;
- the exdata recorder load being non-CSEable and non-hoistable;
- no safepoint inside a new critical ownership/root/publication sequence;
- no process-wide lock introduced for timezone, UTF-8, packing, or patterns;
- no accidental treatment of opaque exdata as a GC reference.

Run TSAN on the exdata stress fixture and the relevant existing threading
drivers. Run ASan/UBSan where supported for NUL-pattern bounds, malformed UTF-8,
and pack/unpack arithmetic.

## Explicit non-import list

Do not spend implementation time re-importing these MoonJIT changes; the
branch review found current equivalents or later replacements:

- setupvalue barrier (`e613105ca` in the current lineage);
- embedded bytecode loader (`820339960`);
- print/tostring fix (`46a1b268e`);
- lexer overflow (`16e5605ee`);
- bytecode dump unpatch (`20ac817a7`);
- FFI hardcoded hashes (`70f4b15ee`);
- C bytecode generation (`62903bacf`);
- `dlerror` handling (`478bcfe52`);
- cdata length/concatenation (`cc4bbec48`);
- debug `getinfo` validation (present in current `src/lj_debug.c`);
- conversion folding (present in current `src/lj_opt_fold.c`);
- GC64 unsinking (present in current `src/lj_snap.c`);
- profiler/finalizer changes already incorporated into current profiler/GC2;
- old FFI parser hash matching, replaced by current direct string comparison;
- old string hash changes, replaced by the current seeded sparse/dense hash;
- MoonJIT table fixes that were reverted or superseded by LuaJITMT's current
  table topology;
- s390x, PPC64, MIPS64, ARM, and AArch64 changes, which are outside the stated
  supported architecture scope and are based on stale backends.

## Definition of done

The extraction is complete only when all of the following are true:

- Every High- and Medium-priority candidate listed in Purpose and scope is
  implemented with the exact contracts in this file.
- No Low-priority candidate has slipped into the diff.
- New APIs compile from external public headers, link, and are exported from
  shared libraries.
- Old API and bytecode-tool defaults remain compatible.
- All focused `compat_*` cases and the `compat_extensions` aggregate pass.
- The stock suite and existing affected MT/JIT/GC suites pass.
- Default, Lua 5.2 compatibility, no-JIT, no-FFI, assertion, shared, static,
  and amalgamated profiles pass as applicable.
- Exdata has passed a race-detector stress run and traced reads demonstrably
  observe concurrent updates.
- Malformed NUL patterns, UTF-8, and packing inputs have passed sanitizer-backed
  bounds/UB checks where supported.
- Linux, Windows, and macOS release checks pass, or unavailable non-Linux gates
  are reported explicitly as outstanding rather than silently waived.
- The final diff contains no generated `src/luajit.h`, packaging churn, CI-only
  changes, documentation rewrites outside this implementation handoff, or
  unrelated formatting.

When handing off the implementation, report the files changed, behavior added,
commands run and their results, sanitizer/race-detector results, platform gates
that were actually exercised, and any remaining unsupported runner rather than
just stating that the work is done.
