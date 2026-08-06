# yabassanshiro (fork)

This is a fork of [devMiyax's YabaSanshiro](https://github.com/devmiyax/yabause) — itself a
libretro-focused fork of [Yabause](https://github.com/Yabause/yabause), the Sega Saturn emulator —
targeting **real-time performance on low-power ARM handhelds** (specifically the R36S) and **static
recompilation tooling** for downstream Saturn decompilation projects. Upstream badges/links are
kept at the bottom of this file; everything above is specific to this fork.

## Why this fork exists

Running Saturn emulation in real time on a Cortex-A53-class handheld is tight even before you add
libretro overhead. Profiling on real R36S hardware surfaced two separate problems:

1. **Single-thread bottleneck.** SH-2 (master + slave), SCU DSP, VDP1 command processing, and
   SCSP/M68K sound all executed inline, one after another, on the same thread as the frontend's
   render loop — leaving a second/third/fourth CPU core sitting idle on a device that needs every
   cycle it can get.
2. **A long-standing audio bug.** SCSP/M68K had run on its own OS thread since day one, paced by a
   wall-clock `nanosleep` loop racing against the main thread with no real synchronization
   discipline — the root cause of this project's long history of audio glitches (echo, duplicated
   dialogue, A/V drift).

Fixing (1) by naively parallelizing everything and fixing (2) turned out to require *opposite*
strategies for different subsystems — see below.

## What was actually done

### Threading: give idle cores real work, safely

Four subsystems were moved off the main emulation thread onto their own dedicated worker threads,
each gated behind its own opt-in build flag (all off by default — this is real, measured,
in-progress work, not something to silently turn on for every user yet):

| Subsystem | Flag | Worker |
|---|---|---|
| SCSP + M68K sound | `YAB_SCSP_WORKER` (needs `YAB_SYNC_SCSP=1`) | `sound_worker.cpp` |
| VDP1 command-list parsing / vertex-texture prep | `YAB_VDP_WORKER` | `vdp_worker.cpp` |
| SH-2 Slave CPU | `YAB_SH2_SLAVE_WORKER` | `sh2_slave_worker.cpp` |
| SCU DSP interpreter | `YAB_SCU_DSP_WORKER` | `scu_dsp_worker.cpp` |

All four share one generic `Dispatcher` (`dispatcher.h`, extracted from the first of these,
`sound_worker`, once the pattern proved out) with two primitives: **Post** (enqueue, return
immediately — for fire-and-forget work) and **Call** (enqueue, block on a future — when the caller
needs a result back). A single worker thread drains its queue in strict FIFO order, which is what
makes Post-then-later-Call safe with zero extra locking: anything posted earlier is guaranteed to
have already run by the time a later job executes.

Getting there safely required fixing real, pre-existing bugs the added concurrency exposed rather
than caused:

- **devMiyax's SH-2 dynarec wasn't thread-safe.** Its `CurrentContext` was a shared
  `thread_local`-via-`dlopen` pattern that's genuinely unsafe under `dlopen`'s forced
  general-dynamic TLS model, and its JIT block cache had no locking at all. Fixed with a real
  `pthread_key_t` and `std::atomic<Block*>` lock-free-fast-path-plus-double-checked-locking cache —
  needed *before* Master and Slave could safely run on separate threads at all.
- **A reentrant `SH2Exec()` call (the FRT input-capture cross-trigger) corrupted CPU state.** It
  left `CurrentContext` pointing at the wrong core mid-call, corrupting that core's cycle
  accounting. Fixed as part of the same dynarec work.
- **Two real deadlocks**, found in testing, not in theory: an unconditional barrier wait on a
  worker that's only started when a game turns its hardware on (hung on frame 1 for the common
  case), and a joinable worker thread never stopped on the normal shutdown path (hung on quit
  instead of exiting cleanly).
- **A genuine data race**, root-caused by bisection after a batching change exposed it: Master's
  own execution could now genuinely overlap in wall-clock time with the Slave worker thread,
  including a reentrant cross-trigger call — two threads executing the same CPU's state at once.
  Fixed with a recursive mutex around all Master/Slave execution.
- **VDP2 compositing was tried and reverted.** Unlike VDP1's command-list parsing, VDP2's draw path
  makes real `gl*` calls partway through (`YglUpdateColorRam`) — posting that to a thread with no
  GL context just crashes. Left as a documented dead end for whoever picks it up next; it needs a
  real split of that function first, not a naive move.

Dispatch cadence was tuned down as far as it would go without breaking cycle accuracy: the SH-2
Slave/SCU DSP worker wake-up rate went from ~2600/frame (once per scanline) to ~260/frame (once per
frame, flushed at VBlankIN, strictly before the render pass reads VRAM/registers) — each wake-up of
a sleeping worker thread is real, measurable cost, not free just because it's "fire-and-forget."

### Audio: the opposite fix — remove the thread entirely

Deep investigation (two runs of the identical binary producing measurably different audio output)
confirmed the SCSP/M68K async thread's races were real, not theoretical, and that patching it to be
race-free was a bigger, riskier undertaking than it looked — SoundRAM is the single hottest shared
path in the whole emulator. Instead: SH-2/SCU/SMPC/CD-block were already cycle-driven,
single-threaded, and deterministic. SCSP/M68K now follow that exact same model instead
(`YAB_SYNC_SCSP=1`). This wasn't new code to write — the synchronous path already existed as a
dead preprocessor fallthrough in `ScspExec()`, apparently added once and never enabled. The rest of
the fix was disabling the core's own legacy wall-clock frame limiter under this mode, so RetroArch's
own `audio_sync` backpressure is the only real-time governor in play — exactly like every other
well-behaved libretro core. Result: 60fps, bit-perfect audio sync, zero crackle, confirmed against
real dialogue/FMV playback, even on the plain SH-2 interpreter with no dynarec at all.

Both paths (async thread / cycle-driven sync) coexist behind the flag; the async thread stays the
default until `YAB_SYNC_SCSP` is validated further on real R36S hardware.

### DSP, SCSP, memory-cache and VDP2 CRAM optimization

Additional targeted optimization passes: SCU DSP interpreter improvements, SCSP audio path
optimization, SH-2 memory cache tuning, inlining VDP2 Color RAM access, and disabling redundant
busy-loop/profiling code paths that were costing real cycles for no behavioral benefit.

### Tracing and profiling: turning this emulator into ground truth for static recompilation

Two complementary, both off-by-default, both zero-cost-when-disabled instrumentation layers, built
specifically to support decompilation/static-recompilation projects that need to know *what a real
game actually does* rather than guess from the SH-2 manual and hardware docs alone:

- **`TRACE_INTERP_PC`**: logs each new (never-before-seen) master SH-2 PC address per frame, and on
  any frame with new addresses, dumps a paired `frameN.png` (a `glReadPixels` grab of the current
  HW-rendered framebuffer) and `frameN.txt` (the new addresses) — cheap, incremental
  "PC-coverage-so-far" checkpoints for offline study of what code runs when, cross-referenced
  against what's actually on screen at that moment.
- **`PORTAL_TRACE`**: a fuller, structured real-time capture. Hooks every resolved `JSR`/`JMP`/
  `BRAF`/`BSRF`/`BSR`/`RTS`/`RTE` in `sh2int.c`'s dispatch loop (the opcode-family test itself is
  kept fully inline — an earlier, unconditional per-instruction version was bisected to desync
  CD-block read-latency emulation badly enough to cause a black-screen regression) and every
  VDP1/VDP2/SCU/SMPC register write, streaming one JSON object per line to `$PORTAL_TRACE_PATH`:
  call events include a full R0-R7/PR register snapshot at call time. Toggle capture on/off live
  with the **F9** key (via the libretro keyboard callback) — stop cleanly finalizes the current
  segment, start begins a genuinely new file, so each on/off cycle is its own self-contained
  recording instead of one ever-growing multi-GB log. Set `PORTAL_TRACE_SKIP_BIOS=1` to drop
  everything inside the BIOS ROM window when BIOS fidelity isn't the point. For calls landing past
  the statically-known main-executable address range (a strong sign of a dynamically CD-loaded
  overlay a static disassembler has no way to identify on its own), it additionally dumps a chunk
  of the *live* memory at that address — real ground truth for exactly the case a static analysis
  tool can't resolve by itself.

Both flags compile to nothing when disabled — real production builds pay zero cost for either.

## Build flags summary

| Flag | Default | What it does |
|---|---|---|
| `YAB_SCSP_WORKER` | off | SCSP/M68K sound on its own worker thread (needs `YAB_SYNC_SCSP=1`) |
| `YAB_SYNC_SCSP` | off | Cycle-driven single-thread SCSP/M68K sync (fixes audio races/crackle) |
| `YAB_VDP_WORKER` | off | VDP1 command-list parsing on its own worker thread |
| `YAB_SH2_SLAVE_WORKER` | off | SH-2 Slave CPU on its own worker thread |
| `YAB_SCU_DSP_WORKER` | off | SCU DSP interpreter on its own worker thread |
| `TRACE_INTERP_PC` | off | Per-frame incremental PC-coverage + screenshot dump |
| `PORTAL_TRACE` | off | Structured real-time JSONL call/register-write capture (see above) |

---

## Upstream

[![Travis CI Build Status](https://travis-ci.org/devmiyax/yabause.svg?branch=master)](https://travis-ci.org/devmiyax/yabause)
[![Appveyor Build status](https://ci.appveyor.com/api/projects/status/27foxtv7thxgvu5k/branch/master?svg=true)](https://ci.appveyor.com/project/devmiyax/yabause)
[![Discord](https://img.shields.io/discord/559158456515559424.svg?label=&logo=discord&logoColor=ffffff&color=7389D8&labelColor=6A7EC2)](https://discord.gg/aRJhTBH)
[![Donate](https://liberapay.com/assets/widgets/donate.svg)](https://liberapay.com/~32349/donate)

[![Snap Status](https://build.snapcraft.io/badge/devmiyax/yabause.svg)](https://build.snapcraft.io/user/devmiyax/yabause)
