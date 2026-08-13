# R36S (RK3326) — Next Moves for 60fps on Magic Knight Rayearth

## Device Profile

| Property | Value |
|----------|-------|
| **SoC** | Rockchip RK3326 |
| **CPU** | 4× Cortex-A35 @ 1.512 GHz (max) |
| **GPU** | Mali-G31 (Bifrost, single-core, no compute shaders) |
| **GPU freq** | 400 MHz (max 520 MHz available) |
| **RAM** | 1 GB DDR3 |
| **Kernel** | 4.4.189 (Bifrost Mali driver) |
| **GL** | OpenGL ES 3.0 (Mali G31, no GL 4.x, no compute shaders) |
| **Frontend** | retrorun3 (lightweight RetroArch alternative) |
| **Core path** | `/home/ark/.config/retroarch/cores/yabasanshiro_opt_libretro.so` |
| **Current core** | 2.16 MB, built 2026-08-13, AArch64 DYNAREC enabled |
| **Governor** | `ondemand` (perfmax sets `performance` at launch) |

## What's Already Been Done (last 2 commits)

### Commit 1 (`750a561d`) — Major structural changes:
- **Profiler disabled** (`DONT_PROFILE` in Makefile) — was burning ~1.5M `clock()` calls/sec
- **M68K sync removed** — `M68KSync()` was a dead no-op called 2600×/frame
- **SCSP async thread fixed** — spin-loop now sleeps instead of pegging a core
- **Atomic ordering downgraded** — `seq_cst` → `release/acquire` for m68k_counter
- **Slave SH-2 batched** — accumulated per-frame instead of per-deciline calls
- **Texture atlas cross-frame cache** — skip rebuild when VRAM/registers unchanged
- **Memory bus optimized** — `GET_MEM_CYCLE_R/W` replaced with table lookup
- **Idle-loop detection re-enabled** — `EXEC_FROM_CACHE` decoupled from idle skip

### Commit 2 (`4bf503c9`) — Current diff:
- **MSB-shadow count moved** from texture decode to command dispatch (correctness fix)
- **CRAM no longer invalidates atlas** on palette fades (unless mode 3 active)
- **RBG VRAM upload gated** on dirty flags (skips 512KB memcpy when nothing changed)
- **Atlas dirty-row tracking** — only upload changed rows, not full atlas

## Current Performance

- **60 fps** in simple/indoor areas
- **47–57 fps** in heavy areas (likely outdoor scenes with many sprites + RBG rotation)

## Bottleneck Analysis

### 1. GPU is the Likely Bottleneck in Heavy Areas

The Mali-G31 is a **single-core** Bifrost GPU with:
- No compute shader support (GLES 3.0 only, not 3.1)
- 400 MHz (520 MHz available but not used)
- `simple_ondemand` governor (may not ramp up fast enough)

The RBG (rotated background) path in `ygl_texture.cpp` has a **CPU fallback** when compute shaders aren't available — and on GLES 3.0, they aren't. This means every pixel of the rotated background is computed on the CPU, per frame. For a game like *Magic Knight Rayearth* with its pre-rendered backgrounds, this is the most likely culprit.

**Check this**: The `g_rbg_use_compute_shader` option is 0 by default in `libretro.c:1000`. On GLES 3.0 (Mali G31), compute shaders aren't available, so the CPU fallback in `Vdp2DrawRotation_in()` runs. This is a full per-pixel loop over the entire RBG layer.

### 2. SH-2 Interpreter Still Dominates

Even with the dynarec enabled (`USE_AARCH64_DRC=1`), the interpreter is still the fallback for code that the JIT can't handle. The idle-loop detector was re-enabled, but it's conservative and may not catch all spin-wait patterns.

### 3. Texture Atlas Upload Still Happens

The dirty-row tracking helps, but the atlas is still uploaded every frame when content changes. On a single-core Mali G31, texture upload bandwidth is limited.

### 4. SCSP Async Thread Still Costs

The spin-loop was fixed to sleep, but the async model still costs:
- Atomic counter handshake (release/acquire, cheaper now but not free)
- Wake-up syscalls (YabThreadUSleep)
- SoundRam cache-line ping-pong between cores

## Recommended Next Moves (Priority Order)

### 🔴 P0: Enable GPU Turbo / Performance Governor

The GPU is at 400 MHz with `simple_ondemand`. The device has 520 MHz available.

```sh
# Force GPU to max frequency
echo 520000000 > /sys/class/misc/mali0/device/devfreq/ff400000.gpu/max_freq
echo 520000000 > /sys/class/misc/mali0/device/devfreq/ff400000.gpu/min_freq
echo userspace > /sys/class/misc/mali0/device/devfreq/ff400000.gpu/governor
```

**Impact**: Could be 5-15% GPU uplift. **Zero code changes.**
**Risk**: None (thermal throttling will protect the device).

### 🔴 P1: Switch to Synchronous SCSP (ASYNC_SCSP off)

The synchronous SCSP path is fully present in the tree — it was just never compiled. The comment in `scsp.h:106-122` explicitly says:

> "The synchronous path is fully present in this tree... Turning it on makes SCSP cycle-driven and deterministic, exactly like SH-2/SCU/SMPC/CD already are, and deletes the whole cross-thread handshake."

**What to do**: Comment out `#define ASYNC_SCSP` in `scsp.h:122`. This:
- Eliminates the atomic counter handshake entirely
- Eliminates the SCSP worker thread (no more wake-up syscalls)
- Makes audio deterministic and synchronous
- Frees one core entirely (the SCSP thread was pinned to core 0-1)

**Impact**: Could be 10-20% CPU reduction. The SCSP thread was measured as ~30% of emulator cost before the sleep fix; even after the fix, the handshake + wake-up overhead remains.

**Risk**: Low-Medium. The synchronous path is fully present and was the original design. Audio glitch risk if the main thread can't keep up, but on a 4-core device with dynarec, this should be fine.

### 🔴 P2: Optimize RBG CPU Fallback Path

The RBG rotation background is the most expensive single operation in heavy scenes. On GLES 3.0 (no compute shaders), it runs entirely on the CPU.

**Options** (in order of effectiveness):

1. **Reduce RBG resolution** — `g_rbg_resolution_mode` can be set to `RBG_RES_ORIGINAL` (320×224) instead of higher resolutions. Check if it's already at minimum.

2. **Skip RBG update when nothing changed** — Already partially done (the VRAM upload is gated), but the per-pixel draw loop still runs. Add a dirty check at the top of `Vdp2DrawRotation_in()` to skip the entire loop when the RBG content hasn't changed.

3. **Use a GLES 3.0 fragment shader instead of compute shader** — The compute shader path requires GLES 3.1. But the same logic could be implemented as a full-screen fragment shader pass on GLES 3.0. This would move the per-pixel work from CPU to GPU.

4. **Reduce RBG sample rate** — Sample every other pixel and bilinear-filter. This is a visual quality trade-off but could halve the CPU cost.

### 🟡 P3: Texture Atlas Optimization

The atlas dirty-row tracking helps, but:
- The atlas is 2048×1024 RGBA8 = 8 MB per upload
- Even partial uploads cost PBO mapping + texSubImage2D
- On Mali G31, texture uploads are synchronous (no dedicated DMA engine for this)

**Options**:
1. **Reduce atlas size** — 2048×1024 is generous. Try 1024×1024 or 2048×512.
2. **Use a smaller texture format** — GL_RGB565 instead of GL_RGBA8 (half the bandwidth).
3. **Batch uploads** — Instead of uploading per-frame, upload every N frames for static content.

### 🟡 P4: SH-2 JIT Coverage

The AArch64 dynarec is enabled, but:
- Not all instructions are JIT-compiled (falls back to interpreter)
- The idle-loop detector may not catch all spin-wait patterns
- Some games use self-modifying code that flushes the JIT cache

**Options**:
1. Profile which SH-2 instructions still fall through to the interpreter
2. Add JIT support for the most common missing instructions
3. Tune the idle-loop detector for this game's specific polling patterns

### 🟢 P5: Memory Bus Micro-Optimizations

The memory bus was already optimized (table lookup instead of switch chain), but:
- The `cycle` output-parameter convention still adds overhead
- Each memory access goes through `MappedMemoryReadByte/Word/Long` with indirect function calls

**Options**:
1. Inline the most common memory access patterns
2. Use `CurrentSH2->cycles += cost` directly instead of pointer parameter
3. Add a small "last page" cache for instruction fetch (as documented in `once_a_frame.md`)

## Testing Methodology

```sh
# Build for device
make -C yabause/src/libretro platform=arm64_cortex_a53_gles3

# Deploy
sshpass -p ark scp yabause/src/libretro/yabasanshiro_libretro.so \
  ark@192.168.0.14:/home/ark/.config/retroarch/cores/yabasanshiro_opt_libretro.so

# Run with perfmax and FPS counter
sshpass -p ark ssh ark@192.168.0.14 \
  "sudo perfmax performance /roms2/saturn/Magic_Knight_Rayearth.chd && \
   retrorun3 -c /home/ark/.config/retrorun.cfg --triggers \
   -s /roms2/saturn -d /roms2/bios \
   /home/ark/.config/retroarch/cores/yabasanshiro_opt_libretro.so \
   /roms2/saturn/Magic_Knight_Rayearth.chd"

# Quick perf check (run for 5 seconds, sample CPU)
sshpass -p ark ssh ark@192.168.0.14 \
  "timeout 5 retrorun3 ... & sleep 2 && ps -o %cpu -C retrorun3"
```

## Summary

| Priority | Change | Est. Gain | Risk | Effort |
|----------|--------|-----------|------|--------|
| **P0** | GPU max freq + performance governor | 5-15% | None | 1 line |
| **P1** | Disable ASYNC_SCSP (sync audio) | 10-20% | Low | 1 line |
| **P2** | Optimize RBG CPU fallback | 5-25% | Medium | Days |
| **P3** | Texture atlas format/bandwidth | 3-10% | Low | Hours |
| **P4** | SH-2 JIT coverage | 5-15% | High | Weeks |
| **P5** | Memory bus micro-opt | 2-5% | Medium | Days |

**My recommendation**: Start with **P0 + P1** (zero code changes beyond a `#define` toggle). If those don't close the gap, move to **P2** (RBG optimization) which is the most likely remaining CPU bottleneck on the GPU-limited R36S.
