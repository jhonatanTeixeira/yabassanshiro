# YabaSanshiro — Sega Saturn Emulator (libretro fork)

## Project Overview

**YabaSanshiro** is devMiyax's libretro-focused fork of [Yabause](https://github.com/Yabause/yabause), the Sega Saturn emulator. The `main` branch tracks upstream/libretro releases and is the primary CI target.

The project is a **Sega Saturn emulator** — a complex C/C++ codebase that emulates the Saturn's dual-SH-2 CPU architecture, SCU DSP, VDP1/VDP2 video, SCSP audio, CD block, SMPC, and peripherals. The libretro core (`yabause/src/libretro/`) is the primary build target, producing `yabasanshiro_libretro.so`.

### Current Objective

Cut interpreter CPU usage from ~32% to ~10% of one core (x86_64, RetroArch, `platform=unix` build — plain SH-2 interpreter, no dynarec, no worker threads). See `CLAUDE.md` for the full brief.

### Key Technologies

- **Language**: C (primary), C++ (OpenGL renderer, texture cache, atomic counters)
- **Build system**: Make (libretro core), CMake (desktop frontends)
- **Rendering**: OpenGL ES 2.0+ (`vidogl.c`, `ygl.h`, `ygl_texture.cpp`, `ygles.c`)
- **CPU emulation**: SH-2 interpreter (`sh2int.c`), with ARM/AArch64 JIT backends (`sh2_dynarec_devmiyax`)
- **M68K**: Musashi or C68K (interchangeable, selected via `HAVE_MUSASHI`)
- **Audio**: SCSP + M68K, async thread (`ASYNC_SCSP`) or synchronous path
- **CI**: GitLab CI (libretro templates), Travis CI, AppVeyor

## Architecture

### Directory Layout

```
yabause/
├── src/
│   ├── libretro/          # Primary build target — libretro core
│   ├── qt/                # Qt desktop frontend
│   ├── gtk/               # GTK desktop frontend
│   ├── sdl/               # SDL frontend
│   ├── glfw/              # GLFW frontend
│   ├── android/           # Android port (JNI)
│   ├── ios/               # iOS port
│   ├── cocoa/             # macOS Cocoa port
│   ├── nx/                # Nintendo Switch port
│   ├── dreamcast/         # Dreamcast port
│   ├── runner/            # Headless runner
│   ├── webinterface/      # Web-based UI
│   ├── retro_arena/       # RetroArena-specific build
│   ├── musashi/           # Vendored M68K core
│   ├── c68k/              # Alternative M68K core
│   ├── sh2_dynarec/       # Older ARM-only SH-2 JIT
│   ├── sh2_dynarec_devmiyax/ # Primary SH-2 JIT (ARM/AArch64)
│   ├── libchdr/           # CHD format support
│   ├── libretro-common/   # libretro shared code
│   ├── json/              # JSON parser
│   ├── nanovg/            # Vector graphics library
│   ├── titan/             # Software compositor (vidsoft only)
│   ├── aosdk/             # Android SDK helpers
│   ├── q68/               # Q68 M68K core
│   ├── shaders/           # GLSL shaders
│   ├── tools/             # Standalone diagnostics
│   ├── gameshw/           # Game-specific hardware hacks
│   ├── gdb/               # GDB stub
│   ├── gllibs/            # GL helper libs
│   └── gtk/               # GTK resources
├── CMakeLists.txt         # Desktop frontend build
├── doc/                   # Upstream docs
├── l10n/                  # Translations
└── res/                   # Resources
yabauseut/                 # Hardware conformance test ROMs
mini18n/                   # Vendored i18n library
win_template/              # Windows packaging
snap/                      # Snap packaging
docs/                      # Performance optimization docs
```

### Core Emulation Loop

`YabauseEmulate()` in `yabause.c` is the heart of the emulator. It advances in **decilines** (1/10 of a scanline), interleaving:

- `SH2Exec()` for Master and Slave SH-2 CPUs (every deciline)
- `ScuExec()` (every deciline)
- `Vdp2HBlankIN()`/`Vdp2HBlankOUT()` (every 10th deciline = per scanline)
- `ScspExec()` (every 10th deciline)
- `SmpcExec()` (every deciline)
- `Cs2Exec()` (every deciline)
- `M68K` sync (per deciline, async or sync mode)

At `LineCount == MaxLineCount`, VDP1/VDP2 rendering fires (once per frame).

### Key Subsystems

| Subsystem | File(s) | Description |
|-----------|---------|-------------|
| SH-2 (CPU) | `sh2int.c`, `sh2core.c`, `sh2cache.c` | Dual SH-2 interpreter + JIT backends |
| SCU | `scu.c` | DMA controller + DSP |
| VDP1 | `vdp1.cpp` | Sprite/polygon command processor |
| VDP2 | `vdp2.cpp` | Background/scroll compositor |
| OpenGL renderer | `vidogl.c`, `ygl.h`, `ygl_texture.cpp`, `ygles.c` | GPU-accelerated rendering |
| Software renderer | `vidsoft.c`, `titan/titan.c` | CPU-only fallback (not used in libretro build) |
| SCSP + M68K | `scsp.c`, `m68kmusashi.c`/`m68kc68k.c` | Audio chip + sound CPU |
| CD block | `cs0.c`, `cs1.c`, `cs2.c`, `cdbase.c` | CD-ROM interface |
| SMPC | `smpc.c` | System management / peripherals |
| Memory bus | `memory.c` | Address decode + dispatch |
| Profiler | `profile.c`/`.h` | Instrumentation (active in release builds — see below) |

## Building

### libretro core (primary target)

```sh
# One-time codegen: generates M68K opcode tables
make -C yabause/src/libretro generate-files

# Build (produces yabasanshiro_libretro.so)
make -C yabause/src/libretro platform=unix        # generic Linux/x86_64
make -C yabause/src/libretro platform=arm64       # aarch64 (Switch/Lakka)
make -C yabause/src/libretro platform=rpi4        # Raspberry Pi 4
make -C yabause/src/libretro platform=rpi5        # Raspberry Pi 5

# Debug/sanitizer builds
make -C yabause/src/libretro platform=unix DEBUG=1
make -C yabause/src/libretro platform=unix DEBUG_ASAN=1
make -C yabause/src/libretro platform=unix DEBUG_UBSAN=1
make -C yabause/src/libretro platform=unix DEBUG_TSAN=1

# Clean
make -C yabause/src/libretro clean
```

### Desktop frontend (CMake)

```sh
cd yabause && mkdir build && cd build
cmake .. -DYAB_PORTS=qt   # or gtk, sdl, glfw
make
```

### Key Makefile Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `platform` | `unix` | Target platform (unix, arm64, rpi4, rpi5, etc.) |
| `DEBUG` | 0 | Debug build |
| `HAVE_MUSASHI` | 1 | Use Musashi M68K core (0 = C68K) |
| `DYNAREC_DEVMIYAX` | 0 | Enable devMiyax SH-2 JIT (ARM/AArch64 targets set this) |
| `USE_ARM_DRC` | 0 | ARM32 JIT |
| `USE_AARCH64_DRC` | 0 | AArch64 JIT |
| `USE_X86_DRC` | 0 | x86 JIT (not yet available) |
| `FORCE_GLES` | 0 | Force OpenGL ES |
| `HAVE_SSE` | 1 | Enable SSE optimizations |
| `FASTMATH` | 1 | Fast math flags |

## Running

```sh
# Needs a libretro frontend (RetroArch)
# BIOS goes in the frontend's system directory, named saturn_bios.bin
retroarch -L yabause/src/libretro/yabasanshiro_libretro.so path/to/game.iso
```

## Testing

No automated unit-test suite is wired into the build. Correctness is validated by running real games/BIOS under RetroArch. The `yabauseut/` directory contains hardware conformance test ROMs (needs an external `sh2eb-elf` Saturn cross-toolchain). `yabause/src/tools/` has standalone diagnostic executables.

## Performance Optimization Docs

The `docs/` directory contains three detailed inventories for the CPU-reduction effort:

- **`docs/every_pixel.md`** — Every function called per-pixel in the renderers (vidsoft.c, vidogl.c, titan.c), with hoistability analysis
- **`docs/once_a_frame.md`** — Work redone more often than once a frame (texture caching, SH-2 interpreter dispatch, memory bus, profiler, etc.)
- **`docs/per_deciline.md`** — Work called per deciline (SCSP busy-spin, atomic ordering, M68KSync dead code, SMPC/CD batching analysis)

## Development Conventions

### Code Style

- C code uses K&R-ish style with 3-4 space indentation (inconsistent across files — follow local file style)
- C++ code (`.cpp` files) uses the surrounding file's convention
- OpenGL code uses `ygl.h` abstraction layer (not raw GL calls)
- Comments are sparse; the codebase is old and carries upstream Yabause heritage

### Key Gotchas

- **Profiler is active in release builds**: `DONT_PROFILE` is never defined in the libretro Makefile, so `PROFILE_START`/`PROFILE_STOP` (linear `strcmp` + `clock()`) runs ~1.5M+ times/sec in the shipping build. Define `DONT_PROFILE` in the Makefile to disable.
- **`EXEC_FROM_CACHE` is unconditionally defined** in `sh2int.c:55`, which disables the idle-loop detector (`sh2idle.c`) as a side effect. The idle detector and the cache-exec feature share the same `#ifdef` guard but are independent concerns.
- **`ASYNC_SCSP` is unconditionally active** on Linux builds (the `#if defined(ARCH_IS_LINUX)` guard in `scsp.h:96` is commented out). This means `M68KSync()` compiles to an empty function body but is still called ~2600-3130×/frame.
- **`yinit.scsp_main_mode = 0`** is hardcoded in `libretro.c:1005`, selecting the async SCSP path. The synchronous path (per-deciline `M68KExec()` + `new_scsp_exec()`) is fully present in the tree but never compiled.
- **`yinit.vidcoretype = VIDCORE_OGL`** is hardcoded — `vidsoft.c` and `titan/titan.c` are compiled into the libretro core but never execute.
- **`yinit.sync_shift = 0`** (zero-initialized, never set in `libretro.c`), so the sub-splitting loop in `yabause.c` is dead code.
- **`OPTIMIZED_DMA`** is undefined everywhere — SCU DMA always funnels through `Vdp1/Vdp2RamWrite*`, so dirty-tracking is reliable.
- **`CACHE_ENABLE`** is commented out — SH-2 cache emulation is not on the hot path.

### Atomic Counter Convention

`m68k_counter` (in `Counter.cpp`) is a single-producer (main thread, once per deciline) / single-consumer (SCSP worker thread) monotonic counter. It uses `memory_order_release`/`memory_order_acquire` (not the default `seq_cst`). Same for `m68k_counter_done` (producer: SCSP worker, consumer: main thread).

### Render Path

The libretro build uses the OpenGL renderer (`vidogl.c` + `ygl_texture.cpp` + `ygles.c`). The software renderer (`vidsoft.c` + `titan/titan.c`) is compiled in but never called. The RBG (rotated background) path uses a compute shader when available (`rbg_use_compute_shader`), falling back to a CPU path on platforms without compute-shader support (e.g., macOS/GL 4.1).

### Texture Cache

The GPU texture atlas is reset every frame (`YglTMReset`/`YglCacheReset` in `VIDOGLVdp1DrawStart`/`VIDOGLVdp2DrawStart`). The cache key is content-addressed (packs VRAM address + palette + mode), so it could persist across frames with VRAM dirty-tracking — but currently doesn't. VDP2 VRAM dirty flags (`A0_Updated`/`A1_Updated`/`B0_Updated`/`B1_Updated`) are correctly set on every write but their consumer branches are empty.

### SCSP Async Thread

The `ScspAsynMainCpuTime` worker thread runs continuously, consuming M68K cycles from the `m68k_counter` atomic. It was historically a pure busy-spin (no backoff), which was fixed to use `YabThreadUSleep(50)` in the wait loop. The thread synchronizes with the main thread via `YabWaitEventQueue`/`YabAddEventQueue` at frame boundaries.
