# R36S Definitive Refactoring Guide — Magic Knight Rayearth at 60 FPS

**Synthesized from**: `docs/r36s_next_moves.md`, `docs/once_a_frame.md`, `docs/per_deciline.md`, `docs/every_pixel.md`, `docs/framebuffer_overhaul_qwen.md`, `docs/framebuffer_overhaul_deepseek.md`, `docs/framebuffer_overhaul_gemini.md`

**Target device**: R36S handheld (Rockchip RK3326, 4× Cortex-A35 @ 1.512 GHz, Mali-G31 Bifrost GLES 3.0, 1 GB DDR3)

**Target game**: *Magic Knight Rayearth (USA)*

**Goal**: sustained 60 fps (frame budget ≤ 16.66 ms for the CPU emulation thread)

---

## 1. Empirical baseline (do not argue with these numbers)

| Metric | Value | Source |
|--------|-------|--------|
| `core_avg` (CPU emulation thread) | **18.63 ms** | retrorun3 `--benchmark`, governors `performance` |
| `video_avg` (GPU presentation) | **4.67 ms** | same |
| `core_avg : video_avg` | **4 : 1** | same |
| GPU utilization | **~8%** | same |
| `g_resolution_mode` | `RES_ORIGINAL` (352×224 / native Saturn) | same |
| Baseline FPS (dynarec, RBG off) | **42.89 fps** | same |
| FPS with RBG compute shader ON | **45.55 fps** | same |
| FPS with SH-2 interpreter (no dynarec) | **35.46 fps** | same |

**Conclusion**: The emulator is **CPU-bound on the main emulation thread**, not GPU-bound. Any plan that starts with "reduce GPU bandwidth" or "lower resolution" is targeting the wrong bottleneck. The GPU has roughly 4× headroom.

Current shortfall: 18.63 ms − 16.66 ms = **1.97 ms** over budget. To hit a safe 60 fps with margin, target `core_avg` ≈ **11–13 ms** (the docs' conservative estimate) or at minimum **≤ 16.0 ms**.

---

## 2. What is already implemented (last two commits)

### Commit `750a561d` — structural loop overhaul
- `DONT_PROFILE` added to Makefile → disabled ~1.5M `clock()`/`strcmp` profiler calls/sec.
- `M68KSync()` dead-call removal (was empty body under `ASYNC_SCSP`).
- SCSP async thread spin-loop fixed to sleep (`YabThreadUSleep(50)`).
- Atomic ordering downgraded from `seq_cst` to `release/acquire` for `m68k_counter`/`m68k_counter_done`.
- Slave SH-2 batched per-frame instead of per-deciline.
- Texture atlas cross-frame cache + dirty-tracking for static content.
- Memory-bus `GET_MEM_CYCLE_R/W` replaced with table lookup.
- Idle-loop detector re-enabled by decoupling it from `EXEC_FROM_CACHE`.

### Commit `4bf503c9` — render-path fixes
- MSB-shadow count moved from texture decode to command dispatch (correctness fix).
- CRAM no longer invalidates atlas on palette fades unless mode 3 active.
- RBG VRAM upload gated on dirty flags.
- Atlas dirty-row tracking → partial `glTexSubImage2D` instead of full atlas upload.

**Result after both commits**: 60 fps in simple areas, **47–57 fps** in heavy areas.

---

## 3. Root-cause summary of the remaining gap

The heavy-area drops are caused by the interaction of four hot spots on the **CPU emulation thread**:

1. **SH-2 interpreter/dynarec fallback** — the AArch64 dynarec is active but falls back to the interpreter for unhandled instructions; the interpreter dispatch loop and memory-access path dominate.
2. **Per-frame texture atlas rebuild** — even with dirty-row tracking, the atlas allocator is still largely reset per frame; static backgrounds/sprite sheets are re-decoded from VRAM.
3. **RBG CPU fallback** — on GLES 3.0 (no compute shaders), rotated backgrounds run a per-pixel CPU loop (`Vdp2DrawRotation_in`).
4. **Memory bus double address-decode** — partially fixed; still has `cycle` output-parameter overhead.

Items that are **not** the problem: GPU fillrate, FBO size, resolution scaling, texture upload bandwidth to GPU.

---

## 4. Definitive prioritized roadmap

### Phase 0 — Zero-code wins (do these first, today)

| # | Change | Where | Est. gain | Risk |
|---|--------|-------|-----------|------|
| 0.1 | Force GPU to 520 MHz and `userspace` governor | `/sys/class/misc/mali0/device/devfreq/...` | 5–15% GPU headroom | None |
| 0.2 | Ensure `performance` CPU governor at launch | `perfmax performance` / `retrorun.cfg` | removes scheduler jitter | None |
| 0.3 | Enable RBG compute shader core option | `yabasanshiro_rbg_use_compute_shader=enabled` | **+2.7 fps / −1.33 ms CPU** | Low |

**Why phase 0 first**: These are validated, reversible, and require no build/deploy cycle beyond a config change. Step 0.3 alone closes two-thirds of the current 1.97 ms shortfall.

---

### Phase 1 — Disable `ASYNC_SCSP` (synchronous SCSP)

**File**: `yabause/src/scsp.h` (~line 122)

**Action**: comment out `#define ASYNC_SCSP`.

**What happens**:
- The SCSP worker thread disappears entirely.
- `M68KExec()` / `new_scsp_exec()` run synchronously per deciline (code already present, currently compiled out).
- Atomic counter handshake and wake-up syscalls vanish.

**Expected gain**: 10–20% CPU reduction on the main thread; frees one core.

**Risk**: Low-Medium. The synchronous path is the original design and is still in the tree. Audio may glitch if the main thread cannot keep up, but with dynarec and 4 cores this is unlikely.

**Verification**: compare `ps -o %cpu` and listen for audio dropouts across multiple games.

---

### Phase 2 — Persistent texture atlas with page-granular dirty tracking

**Goal**: stop throwing away the texture atlas every frame.

**Files**: `yabause/src/vidogl.c`, `yabause/src/vdp2.cpp`, `yabause/src/yglcache.c/.cpp`, `yabause/src/ygl_texture.cpp`

**Implementation**:
1. Move `YglTMReset()`/`YglCacheReset()` from per-frame `VIDOGLVdp1DrawStart`/`VIDOGLVdp2DrawStart` to **init only**.
2. Replace the per-frame allocator rewind with a real free-list/generation allocator, or at minimum a "clear only when VRAM changed" policy.
3. Extend VDP2 VRAM dirty tracking from 128KB bank flags to **4KB page bitmasks** (`u64 vdp2_dirty_pages[2]` for 512KB).
4. Add equivalent VDP1 VRAM 4KB page dirty bitmasks.
5. On cache lookup, re-decode a tile/sprite **only** if a page it depends on (`charaddr`, `paladdr`, Color RAM) is dirty.
6. Gate `RBGGenerator::update()` on the dirty-page mask; skip the 512KB `memcpy` when no page changed.

**Expected gain**: 5–15% on static/menus, 2–5% average in heavy scenes.

**Risk**: Medium. Stale textures are the failure mode. Mitigate with debug asserts that compare decoded pixels against VRAM for a few frames.

---

### Phase 3 — RBG CPU fallback elimination

**Preferred path**: ensure `g_rbg_use_compute_shader=1` is active and working (Phase 0.3).

**Fallback for GLES 3.0 without compute**: implement a GLES 3.0 **fragment-shader RBG pass** as proposed in `framebuffer_overhaul_gemini.md`:
- Bind VDP2 VRAM as a `GL_R8UI`/`RG16UI` texture.
- Pass rotation matrix + tile parameters via UBO.
- Render a fullscreen quad to an offscreen RBG FBO.
- Sample tilemap, character pattern, and Color RAM in the fragment shader.

**Alternative lower-risk trade-off**: reduce RBG sample rate (e.g. render at half horizontal resolution and bilinear upscale) only when `core_avg` exceeds budget.

**Expected gain**: 1.3–3.0 ms CPU when compute/fragment path replaces CPU fallback.

**Risk**: Medium for fragment-shader rewrite; Low for compute-shader enable.

---

### Phase 4 — SH-2 interpreter dispatch optimization

**Target**: reduce interpreter fallback time when the dynarec cannot compile a block.

**Files**: `yabause/src/sh2int.c`, `yabause/src/sh2core.c`

**Implementation** (in order of effort/reward):
1. **Threaded code dispatch** using GCC/Clang `&&label` extension:
   ```c
   static const void* dispatch[256] = { &&op_00, &&op_01, ... };
   #define DISPATCH() goto *dispatch[opcode >> 8]
   ```
   Expected: 15–25% interpreter overhead reduction (~1.5–2.5 ms when interpreter is active).
2. **Instruction prefetch queue**: cache last fetched word; avoids a memory-bus call on sequential fetch.
3. **SLEEP fast-forward**: in `SH2InterpreterExec`, when `SLEEP` is executed, set `cycles = target_cycle` and break instead of looping.
4. **"Last page" fetch hint**: remember last 1MB page's base/mask to avoid the `fetchlist[]` indirect call on every instruction.

**Risk**: Medium. The interpreter is the hottest path; any dispatch change needs broad game testing. Gate threaded dispatch behind `#ifdef __GNUC__` with a switch fallback.

---

### Phase 5 — Memory bus micro-optimization

**Files**: `yabause/src/memory.c`, `yabause/src/sh2int.c`

**Implementation**:
1. Add parallel `ReadCycleList[0x1000]`/`WriteCycleList[0x1000]` arrays populated at `MappedMemoryInit()` time, replacing the sparse `GET_MEM_CYCLE_R/W` switch.
2. Keep the dynamic VDP2-RAM cost path intact (`getVramCycle()`).
3. Change memory accessors from `MappedMemoryReadByte(addr, &rcycle)` + caller accumulation to internal `CurrentSH2->cycles += cost`, dropping the output pointer and the ~40 call-site locals.

**Expected gain**: 1–2 ms.

**Risk**: Medium. Cycle timing is the whole point of this code; a table-population bug silently desyncs A/V. Validate by comparing cycle counts frame-over-frame against the unmodified build.

---

### Phase 6 — Remove remaining dead CPU work

| Work | File | Fix | Est. gain |
|------|------|-----|-----------|
| `cell_scroll_data` fill | `vdp2.cpp:786-789` | Gate behind `VIDCORE_SOFT` or `#if 0` with comment | ~0.5–1 ms |
| `M68KSync()` calls | `yabause.c` per-deciline loop | `#if !defined(ASYNC_SCSP)` around call sites | ~0.3–0.5 ms |
| `Vdp2GetAlpha` redundant `CCCTL` re-read | `vidogl.c:1919-1952` | Use `info->blendmode & VDP2_CC_ADD` | small |
| `Vdp1ReadTexture` repeated `SPCTL` reads | `vidogl.c:564-928` | Hoist `rgbSpritesAllowed`/`spriteType` to locals | small |

**Risk**: Low. These are either dead-code removal or mechanical hoisting of frame-frozen values.

---

### Phase 7 — Framebuffer / GPU-side cleanup (only after Phases 0–6)

Because the GPU is not the bottleneck, framebuffer overhauls are **incremental**, not transformative. Pursue only the lowest-risk items:
1. Dirty-rect tracking for CPU framebuffer writes (replace `cpu_framebuffer_write[]` counter with a rectangle).
2. Lazy allocation of `Vdp1FrameBuffer[]` CPU shadows.
3. Scissored VDP1 erase (`EWDR`) using `glScissor` + `glClear`.

Avoid until necessary: compute-shader framebuffer access, 16-bit framebuffer format rewrite, persistent buffer storage — high risk for small gain on this device.

---

## 5. Expected cumulative impact

| Phase | Δ `core_avg` | Cumulative |
|-------|--------------|------------|
| Baseline after existing commits | — | ~18.6 ms |
| 0.3 RBG compute shader | −1.33 ms | ~17.3 ms |
| 1 Disable `ASYNC_SCSP` | −1.5 to −3.0 ms | ~14.3–15.8 ms |
| 2 Persistent texture atlas | −0.8 to −1.5 ms | ~13.3–14.5 ms |
| 3 RBG fallback eliminated | 0 (already counted) or −0.5 to −1.5 ms if fragment path | ~12.8–14.0 ms |
| 4 SH-2 interpreter dispatch | −1.0 to −2.0 ms | ~11.8–13.0 ms |
| 5 Memory bus micro-opt | −0.8 to −1.5 ms | ~11.0–12.2 ms |
| 6 Dead work removal | −0.5 to −1.0 ms | ~10.5–11.7 ms |

**Target achieved**: `core_avg` well under 16.66 ms, with margin for tail latency.

---

## 6. Verification protocol

### Build/deploy
```sh
make -C yabause/src/libretro platform=arm64_cortex_a53_gles3 -j$(nproc)
sshpass -p ark scp yabause/src/libretro/yabasanshiro_libretro.so \
  ark@192.168.0.14:/home/ark/.config/retroarch/cores/yabasanshiro_opt_libretro.so
```

### Benchmark run
```sh
sshpass -p ark ssh ark@192.168.0.14 \
  "sudo perfmax performance /roms2/saturn && \
   retrorun3 -c /home/ark/.config/retrorun.cfg --triggers \
   --benchmark 20 --benchmark-warmup 5 \
   --benchmark-json /tmp/bench_$(date +%s).json \
   -s /roms2/saturn -d /roms2/bios \
   /home/ark/.config/retroarch/cores/yabasanshiro_opt_libretro.so \
   '/roms2/saturn/Magic Knight Rayearth (USA).chd'"
```

### Primary metrics
- `core_avg` must be **≤ 16.0 ms**.
- `core_p95` should trend **< 25–30 ms** (reduces stutter).
- `video_avg` should remain **< 6 ms** (confirms GPU is still not the bottleneck).
- FPS must hold **60** in simple areas and **≥ 55** in the heaviest outdoor/RBG scenes.

### Regression suite
Run each change against:
- *Magic Knight Rayearth* — target title, RBG + heavy sprite areas.
- *Sega Rally* — rotation coefficient tables.
- *Virtua Fighter 2* — heavy VDP1, MSB shadows.
- *NiGHTS into Dreams* — line-scroll, bitmap modes.
- *Panzer Dragoon* — transparency effects.
- *Shining Force III* — known CPU framebuffer access.

Listen for audio dropouts after Phase 1 (`ASYNC_SCSP`).

---

## 7. Risk register

| Change | Likely failure mode | Mitigation |
|--------|---------------------|------------|
| `ASYNC_SCSP` off | Audio dropouts / crackle | Revert one `#define`; test multiple games |
| Persistent texture atlas | Stale/corrupt textures | Debug asserts; validate against VRAM writes; keep "clear cache" debug key |
| Threaded dispatch | Crash on unsupported compiler | `#ifdef __GNUC__` fallback; test x86 build too |
| Memory bus table | A/V desync, not crash | Frame-cycle comparison against baseline |
| RBG fragment shader | Wrong rotation/perspective | Pixel comparison against CPU fallback |
| GPU overclock | Thermal throttle | Monitor `cat /sys/class/thermal/thermal_zone*/temp` |

---

## 8. What NOT to do

Based on the empirical evidence, these approaches will waste time:
- **Lowering resolution** — already at native Saturn; GPU has 4× headroom.
- **Reducing FBO/texture atlas size** — upload bandwidth is not the CPU bottleneck; CPU decode is.
- **Rewriting the framebuffer to 16-bit** — high risk, small gain, not on the critical path.
- **Adding more worker threads** — the device has 4 small cores; the current async SCSP thread is itself a problem.
- **Optimizing `vidsoft.c` / `titan.c`** — these are compiled into the libretro core but **never execute** (`VIDCORE_OGL` is hardcoded).

---

## 9. Summary

1. **Confirm the bottleneck is CPU, not GPU**, with `retrorun3 --benchmark`.
2. **Apply zero-code wins**: GPU turbo, performance governor, enable RBG compute shader.
3. **Remove the async SCSP thread** by disabling `ASYNC_SCSP`.
4. **Make the texture atlas persistent** with 4KB VRAM dirty-page tracking.
5. **Eliminate the RBG CPU fallback** (compute shader now; fragment-shader fallback later if needed).
6. **Optimize the SH-2 interpreter dispatch** and memory bus only after the above structural wins.
7. **Measure every change** against the same benchmark and regression suite; revert if it does not move `core_avg` toward ≤ 16 ms.

The path to 60 fps on the R36S is clear, sequential, and validated by the existing benchmark data. The remaining work is implementation and testing, not further bottleneck analysis.
