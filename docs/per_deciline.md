# Things called once per deciline

Follow-up to `docs/every_pixel.md` and `docs/once_a_frame.md`, same CPU-reduction effort (target
~32% → ~10% of one core, see `CLAUDE.md`). This one's a distinct granularity: `YabauseEmulate()`'s
main loop in `yabause.c` (lines 646-900) runs once per **deciline** (1/10 of a scanline) — roughly
**2600-3130 times per frame** (NTSC 263 lines × 10, PAL 313 × 10). That's coarser than
per-instruction (hundreds of thousands/frame, covered in `once_a_frame.md`) but much finer than
per-scanline (~262-313×/frame) or per-frame.

Confirmed unconditional per-deciline calls, straight from the loop body:
- `SH2Exec(MSH2, ...)` and `SH2Exec(SSH2, ...)` (yabause.c ~745-757)
- `ScuExec(sh2cycles >> 1)` (yabause.c:809)
- `M68KSync()` (yabause.c:812, plus once more after the loop at yabause.c:860)
- `SmpcExec(...)` (yabause.c:817)
- `Cs2Exec(...)` (yabause.c:820)
- `M68KExec()`/`new_scsp_exec()`/`setM68kCounter()`, depending on sync mode (yabause.c:824-857)

**Not** per-deciline, despite looking like it at a glance: `Vdp2HBlankIN()`/`Vdp2HBlankOUT()`/
`ScspExec()` are gated on `yabsys.DecilineCount==9`/`==10`, so they run once per **scanline**
(~262-313×/frame) — a 10x coarser granularity, already in scope of `once_a_frame.md`'s territory,
not this doc.

## Top findings overall, by estimated impact

1. **The async-SCSP consumer thread busy-spins with zero backoff, permanently pegging one CPU
   core** (`ScspAsynMainCpuTime`, `scsp.c:5525-5535`) — confirmed the *active* path on the
   libretro build (`scsp_main_mode=0` is hardcoded, `libretro.c:1005`). This is a `do { ... }
   while (m68k_cycle == 0)` loop with no `sched_yield`/`usleep`/pause intrinsic, continuously
   polling an atomic counter. On a 4-core ARM handheld this alone can account for a large,
   *constant* fraction of device CPU% — independent of game complexity, independent of
   optimizing anything else in this doc or the other two. **Likely the single biggest win in
   this entire investigation, bigger than anything found in the render path or SH-2 interpreter.**
2. **`setM68kCounter()` uses a full `seq_cst` atomic store** (`Counter.cpp:45-47`) called once
   per deciline (~2600-3130×/frame ≈ 156K-188K/sec) — a `dmb`-class full barrier on ARM, on top
   of being the exact variable the spin-loop above is hammering, so writer and pathological
   reader amplify each other's cache-line-invalidation cost.
3. **`M68KSync()` is a confirmed dead no-op** on this build (`scsp.c:5371-5372`, compiles to an
   empty function since `ASYNC_SCSP` is unconditionally defined) — called ~2600-3130×/frame for
   nothing but call/return overhead.
4. **The profiler again** — `PROFILE_START`/`STOP` around `SmpcExec`/`Cs2Exec` alone account for
   >600,000 profiler calls/sec (linear `strcmp` scan + `clock()` each), on top of everything
   already flagged in `once_a_frame.md`. Same one-line `DONT_PROFILE` fix covers this too.
5. **`SmpcExec`/`Cs2Exec` are both already well early-out'd internally** (unlike some render-path
   findings) — the only real lever there is *call frequency* itself (both dispatchers' actual
   thresholds are coarser than a deciline), but touching that is Medium-High risk for the CD
   block specifically — this codebase has a documented history of exactly this class of
   regression (see below).

---

## SH2Exec per-call overhead (FRT/WDT/DMA)

### `SH2Exec()` (sh2core.c:231-245)
- **Current call frequency**: Twice per deciline (MSH2 always, SSH2 when `IsSSH2Running`).
  Confirmed `yabsys.sync_shift` is **0** on the libretro build (never set in `libretro.c`, and
  `yinit` is zero-initialized) — so the `sync_shift`-driven sub-splitting loop in `yabause.c`
  (lines 738-753) is dead code here: each deciline does exactly one `SH2Exec` call per CPU, not
  several smaller ones.
- **What it does**: `CurrentSH2 = context; SH2Core->Exec(...); FRTExec(cycles); WDTExec(cycles);
  DMAProc(cycles);` — three extra full function calls tacked onto every invocation, ~5200-6260
  extra call triples/frame across both CPUs.

### `FRTExec()` (sh2core.c:2041-2095)
- **Current call frequency**: ~5200-6260×/frame (once per `SH2Exec` call, both CPUs).
- **What it does**: Advances the Free Running Counter, does two threshold compares (OCRA/OCRB)
  and one overflow check, updates FTCSR status flags regardless of whether interrupts are wired.
- **Why *not* simply hoistable**: No dead branch to strip — real hardware has no FRC enable bit
  (always free-runs), and games commonly poll `FTCSR`'s status bits directly rather than only via
  interrupt, so the flag writes can't be skipped even when the game doesn't use FRT interrupts.
  Close to the minimum necessary per-tick work.
- **Proposal**: Marginal only — cache `mask = (1<<shift)-1`, recomputing only on `TCR`/reset
  writes instead of every call (shift rarely changes). The bigger lever is call *frequency*, not
  this function's internals — see "Batching the deciline loop itself" below for why that's risky.
- **Risk**: Low for the mask-cache micro-opt. Medium if frequency is reduced — see below (the
  threshold-compare logic is single-shot per call and can't detect more than one crossing within
  a batched call).

### `WDTExec()` (sh2core.c:2099-2136) — confirmed *not* a finding
Already has the early-out most games hit: `if (!isenable || WTCSR&0x80 || RSTCSR&0x80) return;`
— WDT is rarely enabled by Saturn game code, so the common case is 3 loads + branches. Already
correctly gated.

### `DMAProc()` (sh2core.c:2200-2240) — confirmed *not* a finding for the idle path
Common case (no DMA in flight) is ~4-5 comparisons and a return, matching `once_a_frame.md`'s
existing assessment of the sibling `ScuDmaProc`/`SucDmaCheck`. **Batching note**: unlike FRT/WDT,
its inner transfer loop is self-metering (`copy_clock`/`eat`-based) and proportional to `cycles`
— if `SH2Exec` were ever called with a bigger cycle batch, this part would just iterate more per
call with no change in DMA-completion timing. This part alone would be safe to batch; FRT/WDT are
the parts that wouldn't be (see below).

---

## ScuExec

### `ScuExec()` (scu.c:1308-1349), Timer1 dispatch — confirmed *not* a finding
2-3 loads/branches when Timer1 disabled (the common case). Same category as the existing
`ScuDmaProc` "not a finding."

**Correctness bug spotted in passing, not a performance issue, flagged for a maintainer
separately**: `scu.c:1312`, `if (ScuRegs->T1MD & 0x80 == 0)` — `==` binds tighter than `&` in C,
so this evaluates as `T1MD & (0x80==0)` = `T1MD & 0` = always false. The intended direct-exec
branch is dead; execution always falls through to the `LineCount==T0C`-gated branch instead. This
*reduces* call frequency rather than adding waste, so it's out of scope for this doc, but it's a
real bug worth a separate look.

### SCU DSP instruction loop (scu.c:1350-1971) — confirmed *not* a new finding
Correctly gated behind `ScuDsp->ProgControlPort.part.EX` (DSP-running flag); when active, the
per-cycle work is proportional interpreter work, not fixed overhead. Already covered by
`once_a_frame.md`'s breakpoint-scan finding (scu.c:1370-1376) — nothing new here.

---

## M68K/SCSP sync

### `ScspAsynMainCpuTime` unthrottled busy-spin (scsp.c:5525-5535) — **top overall finding**
- **Current call frequency**: Runs continuously on its own OS thread for the entire session,
  active by default (`yinit.scsp_main_mode = 0` is hardcoded, `libretro.c:1005`).
- **What it does**:
  ```c
  do {
    m68k_integer_part = getM68KCounter() >> SCSP_FRACTIONAL_BITS;
    m68k_cycle = m68k_integer_part - pre_m68k_cycle;
  } while (m68k_cycle == 0);
  ```
  A pure spin loop, zero backoff — no `sched_yield`, `usleep`, `nanosleep`, or pause intrinsic —
  waiting for the main thread's next `setM68kCounter()` call.
- **Why wasteful**: Continuously re-reading an atomic in a hot loop saturates one core at 100%
  regardless of whether there's anything to do. On a 4-core ARM handheld (R36S-class), this alone
  can be a large, constant fraction of total device CPU%, independent of game complexity.
- **Proposal**: Bounded spin-then-block — spin briefly (a few hundred iterations, or a
  `sched_yield()`/pause intrinsic per iteration), then fall back to a condition variable or
  semaphore wait, woken by `setM68kCounter()`. `setM68kCounter` is already called ~2600-3130×/frame,
  so wait windows are already very short; even a cheap `sched_yield()` per spin iteration would
  likely eliminate the 100%-core floor with negligible latency added.
- **Risk**: Low-Medium — pure host-thread scheduling change, doesn't touch emulated timing
  semantics (same `m68k_counter` target honored), but this thread ultimately drives SCSP/M68K
  audio generation, so any wait-primitive swap needs a real audio-glitch regression pass, not
  just a `ps -o %cpu` check.

### `setM68kCounter()`'s `seq_cst` atomic store (yabause.c:855, Counter.cpp:45-47)
- **Current call frequency**: Once per deciline, unconditional — ~2600-3130×/frame ≈
  156K-188K calls/sec.
- **What it does**: `m68k_counter = counter;` on a `std::atomic<u64>` — default assignment is
  `memory_order_seq_cst`, a full barrier (`dmb` on ARM, `mfence`/locked op on x86).
- **Why wasteful**: This is a monotonically-increasing counter with a single writer (main thread)
  and single reader (the spin-loop above) — textbook producer/consumer handoff, not a case that
  needs total ordering. Combined with the busy-spin above, every store here forces cache-line
  invalidation traffic between the two cores on top of the fence cost itself; the write side and
  the pathological spin-read side amplify each other.
- **Proposal**: Downgrade to `memory_order_release` store paired with `memory_order_acquire` load
  in `getM68KCounter()` — the standard correct minimum for single-producer/single-consumer
  counter handoff, cheaper on both x86 and ARM. Pairs naturally with fixing the busy-spin above.
- **Risk**: Low — textbook pattern for this exact use case.

### `M68KSync()` — confirmed dead no-op on this branch (yabause.c:812, 860; scsp.c:5371-5372)
- **Current call frequency**: Once per deciline inside the loop (~2600-3130×/frame) plus once
  more after the loop (~60×/frame).
- **What it does**: `#if defined(ASYNC_SCSP) \n void M68KSync(void){} \n ...` — since
  `ASYNC_SCSP` is unconditionally defined (`scsp.h:96`, the `#if defined(ARCH_IS_LINUX)` guard
  around it is commented out — no build configuration in this tree disables it), this compiles to
  an empty function body. Every deciline pays a real extern-linkage call+return for nothing.
- **Proposal**: `#ifdef ASYNC_SCSP` out both call sites, or make it a header `static inline {}` so
  the compiler can eliminate it without relying on cross-TU inlining/LTO.
- **Risk**: Low — byte-for-byte behavior-preserving; the body is empty in the only compiled
  configuration.

### Which path actually runs per deciline (yabause.c:824-857) — clarifies a `once_a_frame.md` assumption
With `ASYNC_SCSP` unconditionally defined, the preprocessor collapses the whole `#if
!defined(ASYNC_SCSP)` block to dead code — **neither `M68KExec()` nor `new_scsp_exec()` runs at
deciline granularity** in this build. Only `saved_m68k_cycles += m68k_cycles_per_deciline;
setM68kCounter(saved_m68k_cycles);` actually executes per deciline. The real M68K/SCSP execution
happens on the separate `ScspAsynMainCpuTime` worker thread, in 256-cycle (~one audio sample)
batches — already coarse-grained, and out of this doc's per-deciline scope, but worth knowing:
optimizing `M68KExec`/`new_scsp_exec` internals wouldn't touch the deciline loop's cost at all.
(`use_new_scsp` is explicitly forced to `1` by `libretro.c:1002`, which only selects which
function the worker thread calls per sample, not anything at deciline granularity.)

---

## Batching the deciline loop itself

**Direct answer to "is per-deciline granularity load-bearing or just an implementation
artifact?": it's load-bearing, for a specific, checkable reason.**

- Interrupts (FRT compare-match, WDT overflow, SCU DMA-end, SCU DSP-end, Timer0/Timer1) are only
  checked at the top of `SH2InterpreterExec`, i.e. **at `SH2Exec` call boundaries** — so the
  number of `SH2Exec` calls per frame is literally the number of chances per frame a pending
  interrupt gets delivered. Folding decilines together (e.g. batching 10 into 1, to
  scanline granularity) would multiply worst-case interrupt latency by the fold factor — from
  ~91-183 SH2 cycles (~3-6µs) up to ~900-1800 cycles (~30-65µs) worst case.
- Saturn games commonly poll SCU DMA/DSP status registers directly via spin-wait rather than
  interrupts, and that polling executes as SH-2 instructions interleaved with `ScuExec` at the
  *same* deciline granularity. Coarsening `ScuExec` calls would mean a spin-wait loop could
  execute many more "not ready yet" iterations before the polled state actually updates —
  eventually consistent, not a hang, but a real timing-accuracy regression, and exactly the class
  of DMA-synced busy-loop `once_a_frame.md`'s idle-detection finding already flags as the risk
  case to watch for.
- `FRTExec`/`WDTExec`'s threshold-compare logic is single-shot per call (`frctemp >= OCRA &&
  frcold < OCRA`) — it can only detect one register-crossing per call. Current per-deciline cycle
  counts (tens to ~180 cycles) make a double-crossing within one call implausible for realistic
  `OCRA` values; a naive "batch 10 decilines into 1" change would make that far more plausible and
  could silently swallow a compare-match interrupt.

**Conclusion**: don't restructure `SH2Exec`/`ScuExec` call granularity — the interleaving is
load-bearing for interrupt/polling timing accuracy, and the fixed per-call costs inside it
(FRTExec/WDTExec/DMAProc/Timer1-check) are already small relative to the SH-2 instruction
interpretation happening in the same call. The real, safe wins at this layer are the ones above
that don't touch emulated-cycle timing at all: the SCSP busy-spin, the atomic ordering, and
removing the dead `M68KSync()` calls.

**If someone wants to pursue batching anyway**: Medium-High risk, needs per-title regression
testing targeting DMA/DSP-polling-heavy titles and anything using FRT/WDT compare-match
interrupts — mirroring the caution `once_a_frame.md` already raises for the idle-loop-detector work.

---

## SmpcExec

### `SmpcExec` top-level dispatch (smpc.c:552-631) — confirmed already well early-out'd
- **Current call frequency**: Once per deciline, unconditional, ~2600-3130×/frame.
- **What it does**: Decrements a countdown by `t`; on reaching 0, dispatches the pending command
  via `switch(SmpcRegs->COMREG)`.
- **Confirmed idle-case cost**: `SmpcInternalVars->timing` is 0 (idle) the overwhelming majority
  of calls — its only writer is `SmpcSetTiming()`, itself only called once per issued command (a
  COMREG write), not per deciline. In the idle case the whole function is one load, one compare,
  one not-taken branch. Already about as cheap as an early-out gets — **not** the "structure copy
  + switch dispatch paid every call regardless" pattern the render-path findings show.
- **`intback_wait_for_line` / `LineCount == 207` poll** (smpc.c:555-562): real hardware behavior
  (INTBACK's peripheral-data response can't complete before line 207, inside VBlank) — a single
  integer compare, negligible even repeated across the ~800-call wait window. Not a finding.

---

## Cs2Exec / CD block

### `Cs2Exec` top-level dispatch (cs2.c:937-1131) — confirmed already well early-out'd
- **Current call frequency**: Once per deciline, unconditional, ~2600-3130×/frame.
- **What it does**: Two unconditional accumulator adds, a command-lock countdown, a
  command-completion countdown, then two threshold checks gating (a) a CD-tray/disc-presence poll
  and (b) the sector-read/status-report state machine.
- **Confirmed idle-case cost, verified against the actual constants**:
  - Tray-status branch fires roughly once per ~333ms (`_statustiming=1000000` in 3µs units,
    cs2.c:884), not per deciline.
  - Sector-read/status-report branch fires roughly 1-4×/frame (`_periodictiming` 20000-50000,
    cs2.c:1158-1163), not per deciline.
  - `Cs2Area->cdi->GetStatus()` resolves to `ISOCDGetStatus()` (confirmed active core via
    `libretro.c:1301`'s hardcoded `CDCORE_ISO`) — a single variable read, no I/O.
  - Idle-path cost (no command executing, neither threshold crossed — the common case): 2 adds +
    2 not-taken branches + 2 threshold compares + 2 cart-type compares. Same conclusion as SMPC —
    no expensive dispatch paid when idle.
- **Why hoistable at all**: Only the *call frequency* — both dispatchers' own thresholds are
  coarser than one deciline by 10-50,000×, so 9 out of every 10 calls (or far more) do
  provably nothing beyond a handful of cheap compares.
- **Proposal, if pursued**: Batch to once per scanline (accumulate `usecinc*10`, call alongside
  `Vdp2HBlankOUT()`/`ScspExec()` at `DecilineCount==10`), and convert the `_statuscycles`/
  `_periodiccycles` threshold checks from `if` to `while` as a defensive correctness measure so a
  call spanning more than one period never silently drops a cycle. **Ship SMPC and CD-block
  batching as two independent, separately-tested changes — do not couple them.**
- **Risk**: Medium (SMPC) / **Medium-High (CD block)**. This project's own README (on the
  `perf/r36s-improvements` branch, not `main`) documents a real prior regression from touching
  CD-block read-latency timing via an unrelated instrumentation change — the codebase's CD timing
  is fragile even to changes that shouldn't matter. Two concrete numbers back up the caution:
  - SMPC's smallest INTBACK timing constant is 250 units ≈ 83µs, only ~1.3× one scanline
    (63.5µs) — batching to per-scanline could shift controller-polling completion latency by up
    to a full scanline. Not catastrophic, but real for a latency-sensitive path.
  - CD block's `_commandtiming` can be as low as 50 raw units — comparable to or smaller than one
    scanline — and the periodic-cycle checks use `if` not `while`, so naive batching could start
    lagging real sector-read cadence rather than erroring loudly. **Do not touch
    `Cs2ReadFilteredSector`, `Cs2Execute`, or anything in the command-completion path beyond the
    call-cadence change proposed above** — those are squarely in the CD-read-latency danger zone.
  - Aside: `DecilineMode`/`YabauseSetDecilineMode` (yabause.c:519,655,657) looks like it was meant
    for exactly this kind of coarsening, but has zero callers anywhere in the tree, and
    `DecilineCount` still increments once per loop iteration and compares against 9/10 regardless
    of the flag's value — flipping it today would make HBlank/VBlank detection fire ~10× too late
    relative to real emulated time. It's dead and, as far as static reading shows, currently
    broken if enabled. Not a shortcut — any real fix needs new code.

### Confirmed *not* findings in the CD block
- `Cs2ReadFilteredSector` (cs2.c:3934) — genuine per-sector work, only reached from the
  already-gated periodic block at real sector-read cadence (~1-4×/frame). Correctly gated —
  **do not touch**.
- `Cs2Execute()` (cs2.c:1182, full command-dispatch switch) — only runs when `_commandtiming`
  expires, once per completed CD command, not per deciline.
- `doCDReport`/`Cs2SetIRQ` (cs2.c:109-134) — trivial, only reached from already-gated paths.
- CD status-core backends (`DummyCDGetStatus`, `ISOCDGetStatus`) — single-variable reads, no I/O.

### Minor, not worth prioritizing
`Cs2Area->carttype == CART_NETLINK/CART_JAPMODEM` tail check (cs2.c:1127-1130) — two integer
compares every deciline against a value set once at init and never changed on the hot path; could
cache a single `hasSpecialCart` bool, saves one compare in the common case. Genuinely marginal.

---

## Notes for whoever implements these

- Do the SCSP busy-spin fix, the atomic-ordering downgrade, and `M68KSync` removal first — none
  of them touch SH2/SCU/CD-block cycle-accurate state at all, and the busy-spin in particular is
  plausibly worth more than everything else in all three docs combined on a multi-core ARM target.
- `DONT_PROFILE` (already flagged in `once_a_frame.md`) covers the profiler overhead found again
  here around `SmpcExec`/`Cs2Exec` — one fix, multiple docs' worth of payoff.
- The SMPC/CD-block call-frequency batching is real but should be the *last* thing attempted here,
  not the first — smaller payoff than the items above, and the CD-block half specifically carries
  a documented regression history in this exact codebase. If pursued, test with real
  game-compatibility passes (not just `ps -o %cpu`), and land the SMPC and CD-block halves as
  separate, independently revertible changes.
- Per the project's stated approach (`CLAUDE.md`): try it, run the game, compare `ps -o %cpu`
  against baseline, revert with git if something breaks — but for the CD-block item specifically,
  budget real play-testing time before trusting a clean `ps` number alone.
