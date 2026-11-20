# MoonJIT extension optimizations

Created: 2026-11-09, from the `moonjitmt` branch review (base `4f70a776e`).
Status: **open for discussion — the maintainer agrees in principle but wants
to discuss before any work starts.**

Per [15_lockless_completion.md](15_lockless_completion.md), stability comes
first: none of these should start ahead of an open correctness item in
[18_extension_fixes.md](18_extension_fixes.md). Each change needs a before/after
measurement.

## O1. `utf8.codes` decodes every character twice

- **Where:** `utf8_codes_aux` in `src/lib_utf8.c`.
- **Current:** each step decodes the previous character to find where it ends,
  then decodes the current one. Measured 0.107 s for 200,000 two-byte
  characters (`build-msvcrt`, `-jon`).
- **Proposal:** skip continuation bytes from the previous position (as Lua 5.3
  does) and decode only the current character. Expected ~2x.
- **To discuss:** invalid-sequence error positions must stay the same; add a
  test before changing it.

## O2. Byte-at-a-time padding in `string.pack`

- **Where:** `pack_pad` in `src/lib_string.c`.
- **Current:** one `lj_buf_putchar` per padding byte. `string.pack("c100000",
  "")` loops 100,000 times.
- **Proposal:** reserve once with `lj_buf_more` and `memset`.
- **To discuss:** low priority unless large `cN` formats are expected.

## O3. New library functions break JIT traces

- **Where:** `math.type`, `math.tointeger`, `math.ult` (`src/lib_math.c`);
  `string.pack`, `string.packsize`, `string.unpack` (`src/lib_string.c`); all
  of `utf8` (`src/lib_utf8.c`).
- **Current:** they are registered as plain C functions, so the JIT must leave
  the trace to call them.
- **Constraint:** `lj_ffdef.h` lists 248 fast functions, and fast-function IDs
  are one byte (limit 255, enforced in `src/host/buildvm_lib.c`). Only a few can
  be converted.
- **Options to discuss:**
  1. Convert only the functions likely to be hot (`math.type`,
     `math.tointeger`) and record them.
  2. Widen the fast-function ID (it is a `uint8_t` in the function header and
     used by the VM). Large change touching the VM and JIT.
  3. Leave as is until profiling shows a need.
- **Needs first:** a benchmark that calls these in a loop, measured with
  `-jdump` to confirm where traces break.

## O4. `tzset()` cost in `os.date`

Tracked as E8 in [18_extension_fixes.md](18_extension_fixes.md); listed here
because the fix is mainly about cost.

## Questions to settle

1. Which of O1–O3 are worth doing before the next release?
2. For O3, is a wider fast-function ID acceptable in this fork, given the
   divergence from upstream it creates?
3. What benchmark set should gate these (the existing
   `auxiliary/bench/bench.lua`, or new focused microbenchmarks)?
