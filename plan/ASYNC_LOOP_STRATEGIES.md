# Asynchronous Loop Strategies for LuaJIT-MT

Status: exploratory design notes, not an adopted execution plan. Does not
supersede `00_README_AGENT.md` or `15_lockless_completion.md`; any of these
strategies would need to be sequenced behind the open items tracked there.

## TL;DR

- **Strategy A — coroutine reactor (single OS thread, cooperative):** wrap
  stock Lua coroutines around an OS-level I/O poller (epoll/kqueue/IOCP).
  Needs no core VM changes, fully JITable today, and is the standard shape
  used by `copas`/`lua-http`/Node.js-style event loops. **Recommended first
  step** — ships async I/O (sockets, timers, pipes) without touching the
  lockless MT machinery at all.
- **Strategy B — worker-thread pool + channels (real parallelism):** reuse
  the OS-thread primitives already implemented in `lib_threading.c` and the
  bounded MPMC channels in `lj_chan.h`. The main thread runs a loop that
  drains channels with `lj_chan_try_recv`/`_gc` and dispatches Lua
  callbacks. This is mostly *glue*, not new VM engineering, because the
  hard lockless parts (thread lifecycle, GC2 root handoff, safepoints)
  already exist. Best for CPU-bound work; still serializes on the single
  global JIT-recording token (`threading_mt_active_prepare_traces`), so it
  buys parallel *execution* of already-compiled/interpreted code, not
  parallel *tracing*.
- **Strategy C — N:M green threads (coroutines migrated across a worker
  pool):** the highest-payoff, highest-cost option. Would reuse the
  existing `lua_State` ownership-claim protocol (`lj_state_claim`,
  `LJStateOwner`) that worker threads already use to transfer state
  ownership, but generalized so a *suspended coroutine* can be handed to
  whichever OS thread is idle, rather than pinned to the thread that
  created it. This depends on asynchronous-phase GC completion and trace
  stitching, both of which `plan/15_lockless_completion.md` marks open
  (rows "Automatic GC", "Closure construction", "Trace stitching") —
  **defer until those land.**
- **Recommendation:** ship A now, layer B on top using existing primitives
  once A's reactor API is stable, treat C as a v-next item gated on the
  plan/15 GC-completion and trace-stitching work.

---

## 1. What "asynchronous loop" means here

Two distinct things get called "async" and they have different costs in
this codebase:

1. **Concurrency without parallelism** — many logical tasks (I/O waits,
   timers, message handling) interleaved on one OS thread via
   cooperative yielding. This is what Strategy A provides.
2. **Actual parallelism** — tasks running simultaneously on multiple CPU
   cores. This project already has the hard part of this built:
   `lib_threading.c` implements real OS threads with lockless GC2
   integration, safepoint handshakes, and futex-based join/wake. Strategy
   B and C are about exposing that as an *async* (non-blocking,
   callback/future-driven) programming model instead of the current
   blocking `thread:join()` style.

## 2. Strategy A — Coroutine-based single-threaded reactor

### Shape

- A poller abstraction per platform: `epoll_wait` (Linux), `kqueue`
  (macOS), IOCP (Windows) — or start with a portable `select`/libuv-style
  backend and specialize later.
- Each pending I/O operation (socket read/write/connect/accept, timer)
  registers interest with the poller and parks the calling coroutine via
  `coroutine.yield()`.
- A driver loop (`async.run()`): block on the poller with a timeout equal
  to the next timer deadline, then `coroutine.resume()` every coroutine
  whose event fired.
- Timers: a min-heap keyed by deadline, checked once per loop iteration
  (`threading_now_ns()`-equivalent monotonic clock already exists in
  `lj_thr_now_ns()` and is reusable here).

### Why it fits this codebase well

- **Zero interaction with the lockless MT protocol.** Everything runs on
  the single OS thread that owns the interpreter's `lua_State`, so none of
  `mt_active`, `mt_live`, `mt_entering`, or the GC2 root-publication
  machinery in `lib_threading.c` is even invoked. This sidesteps every
  currently-open item in `plan/15_lockless_completion.md` entirely.
- **Fully JITable.** Coroutine resume/yield is a stock, well-traced LuaJIT
  path; none of the "trace stitching disabled" or "one worker token
  serializes tracing" constraints apply, because there is exactly one
  logical thread of control from the JIT's perspective.
- **Small surface area.** Needs one new C module (platform poller +
  timer heap) and a thin Lua-facing library; no changes to `lj_thr.h`,
  `lj_tg.h`, GC2, or `lib_threading.c`.

### Costs / limits

- CPU-bound Lua code still blocks the whole reactor — this strategy only
  helps I/O-bound concurrency, not parallel computation.
- Requires careful cancellation semantics (timeouts must be able to
  de-register a still-pending poller interest).

## 3. Strategy B — Worker-thread pool + channels, non-blocking front end

### Shape

- Spawn a fixed pool of OS threads at startup using the existing
  `threading.spawn`-equivalent path in `lib_threading.c` (already handles
  stack copy, GC2 live-root registration, futex-based start/stop
  handshake — see `threading_worker`, `threading_live_publish`).
- Each worker pulls jobs from a bounded `LJChan` (already implemented:
  `lj_chan_try_recv`/`lj_chan_recv_timeout` in `lj_chan.h`) and pushes
  results to a per-job or shared result channel.
- The *main* thread's async loop (could be the same reactor as Strategy A)
  polls result channels non-blockingly each iteration with
  `lj_chan_try_recv_gc(L, ch, &out)` and resolves the corresponding
  Lua future/promise, resuming any coroutine awaiting it.
- Backpressure and shutdown reuse what's already there:
  `lj_threading_shutdown()` already drains live threads via the futex
  wait loop over `th->state`/`th->futex`; a "future" abstraction is just a
  1-capacity `LJChan` plus a coroutine handle stored alongside it.

### Why this is mostly glue, not new engineering

The genuinely hard lockless-MT problems — safe state ownership handoff
(`threading_worker_claim`, `LJStateOwner`), GC2 root publication across
thread boundaries (`threading_live_publish`, `lj_gc2_preserve_root`), and
graceful join/shutdown under concurrent GC — are already solved in
`lib_threading.c`. Strategy B doesn't need to touch any of that; it needs
a scheduler-side future/promise object and a way to multiplex several
channels into the reactor's poll loop (a plain array scan of
non-blocking `try_recv` calls is sufficient at modest job counts; a
futex-per-channel wakeup could replace polling later if latency matters).

### Costs / limits, tied to current known gaps

- **JIT recording token is global** (`threading_mt_active_prepare_traces`
  calls `lj_jit_token_acquire_wait`): while MT is active, only one TG
  records a trace at a time. Worker threads executing already-compiled
  traces run in parallel fine; workers all trying to *start recording new
  hot loops* at the same time will serialize on that token. This is a
  throughput ceiling, not a correctness issue, but it means Strategy B's
  parallelism gain is best for already-warm code or interpreted work, not
  a guarantee of N-way JIT compilation speedup.
- **Automatic GC is suspended while any MT thread is live**
  (`threading_gc_enter_counted` forces `lj_gc_threshold_store(g,
  LJ_MAX_MEM)` — see the "M4: no automatic GC while children run" comment).
  A worker pool kept alive for the process lifetime effectively disables
  automatic GC for as long as it's running; long-lived pools need an
  explicit periodic `collectgarbage()` policy from the embedding
  application, not the collector's usual heuristics.
- **Trace stitching is disabled** (`plan/15_lockless_completion.md` row
  "Trace stitching": "Production stitch probe rejects every edge and the
  stitch entry returns immediately"). Any design that relies on stitching
  traces across an await/yield boundary inside a worker's hot loop won't
  get that optimization yet; expect an interpreter-speed fallback at
  those boundaries until that item lands.

## 4. Strategy C — N:M green threads (deferred)

### The idea

Instead of pinning each async task's `lua_State`/coroutine to the OS
thread that created it (as in Strategy B, where a job's continuation runs
back on the main thread), let a suspended coroutine be *migrated* to
whichever pool worker is next idle — a true M coroutines : N OS threads
scheduler, closer to Go's goroutines or Erlang's process scheduler than to
a fixed worker pool.

### Why the pieces partially exist already

`lib_threading.c` already implements a state-ownership claim protocol
(`threading_worker_claim`, `lj_state_owner_word_cas`, `LJStateOwner`
packing a `(tid, actor)` pair) so that a `lua_State` can be safely hand
ed from "unowned" to "owned by worker thread T". Generalizing that from
"owned once, for the state's whole lifetime" to "re-claimable every time
the coroutine yields back to `unowned`" is the conceptual leap — the
CAS-based claim/release primitive and the GCSCAN/GCPREP interaction it
already guards against (`lj_state_gcprep_state_acq`) are the same
machinery a migratable-coroutine scheduler would need.

### Why to defer it

- It multiplies the surface area of exactly the protocols
  `plan/15_lockless_completion.md` lists as still open: table iteration
  receiver capture, closure construction fairness, asynchronous GC phase
  ownership, and trace lifecycle/stitching. A coroutine that can resume on
  a *different* OS thread than the one it last ran on stresses all of
  these simultaneously (its stack, upvalues, and any in-flight trace all
  cross a thread boundary on every hop).
- The payoff over Strategy B (a fixed worker pool with channel-based
  dispatch) is mainly *fairness and load-balancing* under many
  short-lived tasks — a real but second-order win compared to just having
  parallelism and non-blocking I/O at all.
- Concretely: build this only after the "Automatic GC" and "Closure
  construction" rows in `plan/15_lockless_completion.md` reach "asynchronous
  phase ownership" completion (their stated target), and after trace
  stitching is re-enabled — both are prerequisites the plan already names
  independent of this proposal.

## 5. Suggested Lua-facing API (illustrative, not final)

```lua
local async = require("async")

-- Strategy A: I/O concurrency on one OS thread
async.run(function()
  local sock = async.tcp_connect("example.com", 80)
  async.write(sock, "GET / HTTP/1.0\r\n\r\n")
  local resp = async.read(sock)
  print(resp)
end)

-- Strategy B: CPU-bound parallel work via the existing thread pool
local pool = async.pool(4)               -- 4 OS worker threads
local future = pool:spawn(function(x) return heavy_compute(x) end, 42)
async.run(function()
  local result = async.await(future)     -- yields until the channel resolves
  print(result)
end)
```

`async.await` on a future is just: park the calling coroutine, register
its resume callback against the future's backing `LJChan`, and let the
reactor's per-iteration channel scan (Strategy B) or poller wakeup
(Strategy A) resume it when data arrives.

## 6. Sequencing recommendation

1. **Now:** implement Strategy A (coroutine reactor + platform poller). No
   dependency on any open plan/15 item; delivers async I/O immediately and
   is fully independent of the MT work.
2. **Next:** implement Strategy B as a thin layer using the existing
   `lib_threading.c`/`lj_chan.h` primitives, documenting the JIT-token and
   suspended-automatic-GC caveats above as explicit operational guidance
   for embedders (e.g., "call `collectgarbage()` periodically if you keep
   a pool alive long-term").
3. **Later, gated:** revisit Strategy C once `plan/15_lockless_completion.md`
   reports the "Automatic GC" and "Closure construction" rows as resolved
   (their own stated completion targets: "asynchronous phase ownership")
   and trace stitching is re-enabled. Attempting it earlier means building
   on top of protocols the plan itself documents as still changing.
