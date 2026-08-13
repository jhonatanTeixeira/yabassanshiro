# R36S Concrete Implementation Plan — Based on Actual Source Code

**Date**: 2026-08-13  
**Branch**: `main`  
**Target**: Magic Knight Rayearth (USA) at sustained 60 fps on R36S (RK3326 / Mali-G31)

---

## Executive summary after reading the code

Most of the optimizations described in the various `docs/*.md` planning files are **already implemented and committed** in the current source tree. The code is in a much more optimized state than the planning docs suggest.

The single largest remaining verified win is:

> **Enable the RBG compute shader by default** — currently disabled (`g_rbg_use_compute_shader = 0`), but the benchmark data shows it gives **+2.7 fps / −1.33 ms CPU**.

After that, the only way to close the remaining gap is empirical profiling on the device, because the obvious structural wins are already done.

---

## What is already done (verified against current source)

| Optimization | Status | Evidence in source |
|--------------|--------|--------------------|
| Profiler disabled (`DONT_PROFILE`) | ✅ Done | `yabause/src/libretro/Makefile:493` defines `-DDONT_PROFILE` |
| `M68KSync()` dead calls removed | ✅ Done | `yabause.c:897-900` wrapped in `#ifndef ASYNC_SCSP` |
| SCSP async thread sleeps instead of spins | ✅ Done | `scsp.c:5525-5549` uses `YabThreadUSleep(50)` and waits for `samplecnt` |
| Atomic ordering release/acquire | ✅ Done | `Counter.cpp:45-65` uses `memory_order_release`/`acquire` |
| Slave SH-2 batched per-frame | ✅ Done | `yabause.c:752-755` accumulates `sh2_slave_cycle_batch`, flushed at VBlankIN |
| SMPC/CD batched per-scanline | ✅ Done | `yabause.c:775-783` accumulates `smpc_cs2_usec_batch` |
| Texture atlas cross-frame cache | ✅ Done | `vidogl.c:6161-6269` compares sanitized VDP1/VDP2 regs + dirty flags |
| RBG VRAM upload gated on dirty flags | ✅ Done | `vidogl.c:6136-6137` + `ygl_texture.cpp:2511-2518` |
| Atlas dirty-row tracking | ✅ Done | `ygles.c:588-604` uploads only `dirty_y0..dirty_y1` |
| Memory bus table lookup | ✅ Done | `memory.c:772-779` `ReadCycleList`/`WriteCycleList` |
| Idle-loop detection re-enabled | ✅ Done | `sh2int.c:3173-3184`, guarded to avoid cache-as-RAM region |
| `SLEEP` fast-forward | ✅ Done | `sh2int.c:3198-3204` |
| `cell_scroll_data` dead fill removed | ✅ Done | `vdp2.cpp:786-789` gated on `VIDCORE_SOFT` |
| CRAM atlas invalidation conditional | ✅ Done | `vidogl.c:6221-6230` only invalidates if SFCCMD mode 3 active |

**Bottom line**: the "Phase 1/2/3" structural refactor from the planning docs is largely complete.

---

## Remaining concrete changes, in order of impact

### 1. Enable RBG compute shader by default (highest impact, lowest risk)

**File**: `yabause/src/libretro/libretro.c:71`

**Current**:
```c
static int g_rbg_use_compute_shader = 0;
```

**Change**:
```c
static int g_rbg_use_compute_shader = 1;
```

**Why**: The benchmark data in `docs/framebuffer_overhaul_qwen.md` proves this gives **+2.7 fps / −1.33 ms** on Magic Knight Rayearth. The code path exists and works:
- `vidogl.c:3670` calls `RBGGenerator_init()` when enabled.
- `vidogl.c:3978` calls `RBGGenerator_update()` instead of the CPU fallback.
- `ygl_texture.cpp:2511-2518` already gates the 512KB VRAM upload on dirty flags.

**Risk**: Low. The option is already exposed to users via `yabasanshiro_rbg_use_compute_shader`; enabling the default just flips the initial value. If a device/driver fails, the user can disable it.

**Verification**: Build and benchmark on R36S; expect `core_avg` 18.6 ms → ~17.3 ms.

---

### 2. Empirically test `ASYNC_SCSP` on vs. off (medium impact, needs measurement)

**File**: `yabause/src/scsp.h:122`

**Current state**: `#define ASYNC_SCSP` is active. The comment at `scsp.h:96-121` explicitly says **KEEP THIS ON for multi-core targets like R36S**, because sync mode moves audio work onto the main thread which is already the framerate ceiling.

**The docs disagree**: `docs/r36s_next_moves.md` and `docs/per_deciline.md` recommend disabling it.

**Reality**: The async thread now sleeps properly and uses release/acquire atomics. The original rationale (busy-spin burning a core) is already fixed.

**Action**: Do **not** change this blindly. Build two cores:
1. Baseline with `ASYNC_SCSP` defined.
2. Test core with `ASYNC_SCSP` commented out.

Run both on R36S with the benchmark harness and compare:
- `core_avg` (main thread)
- Total CPU% (all cores)
- Audio glitches

**Decision rule**:
- If `core_avg` drops **and** no audio glitches → disable `ASYNC_SCSP`.
- If `core_avg` rises or audio breaks → leave it on.

The code comment is probably correct for R36S, but measurement decides.

---

### 3. Profile dynarec fallback instructions (high impact if gaps exist)

**Files**: `yabause/src/sh2_dynarec_devmiyax/` (or whichever DRC is active)

**Why**: With the interpreter already optimized, the next CPU-side win is reducing interpreter fallback in the AArch64 dynarec. The benchmark shows interpreter-only is **24.41 ms** vs dynarec **18.63 ms** — a 5.78 ms gap. If any hot loops are falling out of the dynarec, fixing them is the biggest remaining CPU win.

**How to find them**: Add a counter/logging in `SH2InterpreterExec` that prints the PC and opcode of instructions executed when `SH2Core->id != 2` is false (i.e. when interpreter is active while dynarec is selected). Or use `perf` on the device to sample `SH2InterpreterExec` call stacks.

**Action**: This is a profiling task, not a code change, until the hot missing instructions are identified.

---

### 4. Small render-path cleanups (low impact, low risk)

These are real but small wins. Only worth doing if #1–#3 do not close the gap.

#### 4.1 `Vdp2GetAlpha` redundant `CCCTL` read
**File**: `yabause/src/vidogl.c:1919-1952`

Replace the `fixVdp2Regs->CCCTL` read with a check against `info->blendmode & VDP2_CC_ADD` (already computed per-layer in `Vdp2DrawNBG0` etc.). Skip the function entirely for `specialcolormode == 0`.

**Estimated gain**: small (removes a per-texel branch in the most common mode).

#### 4.2 `Vdp1ReadTexture` repeated `SPCTL` reads
**File**: `yabause/src/vidogl.c:564-928`

Hoist `rgbSpritesAllowed` and `spriteType` to locals at the top of `Vdp1ReadTexture`. `SPCCCS` is already hoisted at line 584; extend the same pattern.

**Estimated gain**: small (removes redundant loads per sprite).

---

### 5. Do NOT pursue these (already optimized or wrong bottleneck)

| Idea | Why skip |
|------|----------|
| Lower resolution / reduce FBO size | GPU is at ~8% utilization; `video_avg` is ~4.7 ms. Not the bottleneck. |
| Persistent texture atlas from scratch | Already implemented in `vidogl.c:6161-6269`. |
| RBG VRAM dirty tracking | Already implemented via `g_Vdp2RamDirtyForRbg`. |
| Memory bus table lookup | Already implemented in `memory.c`. |
| Idle-loop detector | Already re-enabled in `sh2int.c`. |
| `cell_scroll_data` removal | Already gated on `VIDCORE_SOFT`. |
| Framebuffer 16-bit rewrite / compute readback | High risk, small gain; GPU is not the bottleneck. |
| Adding more threads | R36S has only 4 small cores; the async SCSP thread is already a managed trade-off. |

---

## Concrete build/test workflow

### Build baseline (current `main`)
```sh
make -C yabause/src/libretro platform=arm64_cortex_a53_gles3 -j$(nproc)
sshpass -p ark scp yabause/src/libretro/yabasanshiro_libretro.so \
  ark@192.168.0.14:/home/ark/.config/retroarch/cores/yabasanshiro_baseline.so
```

### Build with RBG compute shader default enabled
```sh
# Edit yabause/src/libretro/libretro.c line 71: g_rbg_use_compute_shader = 1
make -C yabause/src/libretro platform=arm64_cortex_a53_gles3 -j$(nproc)
sshpass -p ark scp yabause/src/libretro/yabasanshiro_libretro.so \
  ark@192.168.0.14:/home/ark/.config/retroarch/cores/yabasanshiro_rbg_on.so
```

### Benchmark command
```sh
sshpass -p ark ssh ark@192.168.0.14 \
  "sudo perfmax performance /roms2/saturn && \
   retrorun3 -c /home/ark/.config/retrorun.cfg --triggers \
   --benchmark 20 --benchmark-warmup 5 --benchmark-json /tmp/bench.json \
   -s /roms2/saturn -d /roms2/bios \
   /home/ark/.config/retroarch/cores/yabasanshiro_<variant>.so \
   '/roms2/saturn/Magic Knight Rayearth (USA).chd'"
```

### Metrics to compare
- `core_avg` — must drop toward ≤ 16.0 ms.
- `core_p95` — tail latency, target < 30 ms.
- `video_avg` — should stay < 6 ms (confirms GPU still not bottleneck).
- FPS — target 60 in simple areas, ≥ 55 in heavy areas.
- Audio — listen for dropouts/crackle.

---

## Decision tree after measurements

1. **If RBG compute shader alone reaches 60 fps**: ship just that change. Done.
2. **If still short by < 2 ms**: test `ASYNC_SCSP` off. If it helps without audio issues, ship it.
3. **If still short by > 2 ms**: profile dynarec fallback instructions and fix the hottest missing opcodes.
4. **If still short after all above**: revisit small render-path cleanups (#4) or accept that Magic Knight Rayearth's heaviest scenes are beyond the R36S CPU at stock clocks.

---

## File/line reference table

| Change | File | Line(s) | Status |
|--------|------|---------|--------|
| Enable RBG compute shader default | `libretro.c` | 71 | Not yet done |
| RBG compute shader dispatch | `vidogl.c` | 3670, 3978, 4482 | Already wired |
| RBG VRAM dirty upload | `ygl_texture.cpp` | 2511-2518 | Already wired |
| Texture atlas persistence | `vidogl.c` | 6161-6269 | Already done |
| ASYNC_SCSP toggle | `scsp.h` | 122 | Test both |
| Memory bus cycle tables | `memory.c` | 772-779 | Already done |
| SH-2 idle detection | `sh2int.c` | 3173-3184 | Already done |
| SLEEP fast-forward | `sh2int.c` | 3198-3204 | Already done |
| `Vdp2GetAlpha` cleanup | `vidogl.c` | 1919-1952 | Optional |
| `Vdp1ReadTexture` SPCTL hoist | `vidogl.c` | 564-928 | Optional |

---

## Conclusion

The codebase has already absorbed the major structural optimizations from the planning docs. The next real step is not another big refactor — it is **enabling the existing RBG compute shader path by default**, which is a one-line change with a verified 2.7 fps improvement. After that, measure `ASYNC_SCSP` and dynarec fallback coverage empirically. Do not implement further speculative optimizations until the benchmark data shows they are needed.
