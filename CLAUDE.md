# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

**yabassanshiro**: devMiyax's YabaSanshiro, a libretro-focused fork of Yabause (the Sega Saturn
emulator). `main` tracks upstream/libretro releases (see `.gitlab-ci.yml`, `.travis.yml`,
`appveyor.yml`). `README.md` at the repo root is just upstream CI/donate badges — no project
description lives there on this branch.

## Current objective: cut interpreter CPU usage from ~32% to ~10%

This is emulator code — not production software, not a high-risk target. There is no user data,
no security boundary, no uptime requirement; the worst-case failure mode is a crashed or
graphically-wrong emulator session, immediately visible and trivially reverted via git. Treat it
accordingly: **do not apply production-grade caution here.** No need to hedge changes, keep
fallback paths "just in case," ask before large refactors, or preserve existing structure out of
politeness. If a subsystem needs to be gutted and rebuilt to go faster, gut it. If a comment or an
architectural pattern is in the way of the fastest correct implementation, remove it. Approach this
with a fearless, flip-it-upside-down attitude — the only thing that matters is (a) it still runs the
game correctly and (b) it uses less CPU.

**Baseline**: on this dev machine (x86_64, RetroArch, `platform=unix` build — plain SH-2
interpreter, no dynarec, no worker threads, since none of that exists on `main`), running
*Magic Knight Rayearth (USA)* idles at **~32% of one core** (`ps -o %cpu` on the `retroarch`
process). **Target: ~10%** of one core for the same game under the same conditions, without
changing observable behavior (video/audio output, timing-sensitive game logic).

Validate every change against that same baseline: build, launch under RetroArch with the same
game, and compare `ps -o %cpu` on the `retroarch` process. Use `docs/every_pixel.md` (per-pixel
render-loop inventory) and `perf`/`gprof`/manual `PROFILE_START`/`PROFILE_STOP` instrumentation
(`profile.c`/`.h`) to find where the cycles actually go before optimizing blind. Correctness is
still checked by actually running the game and watching for visible/audible regressions (see
Testing below) — that bar doesn't drop, but everything else about *how* the code gets there is up
for revision.

## Build

The primary, CI-built target is the **libretro core** at `yabause/src/libretro/`. A full desktop
frontend (Qt/GTK/SDL) also exists via the top-level `yabause/CMakeLists.txt`, but the libretro
Makefile is what every CI template (`.gitlab-ci.yml`'s `MAKEFILE_PATH: yabause/src/libretro`) builds
against.

### libretro core

```sh
# One-time codegen: generates Musashi's M68K opcode tables (or C68K's if HAVE_MUSASHI=0)
make -C yabause/src/libretro generate-files

# Build (produces yabasanshiro_libretro.so/.dylib in yabause/src/libretro/)
make -C yabause/src/libretro platform=unix        # generic Linux/x86_64
make -C yabause/src/libretro platform=arm64       # generic aarch64 (Switch/Lakka target)
make -C yabause/src/libretro platform=rpi4        # Raspberry Pi 4
make -C yabause/src/libretro platform=rpi5        # Raspberry Pi 5
make -C yabause/src/libretro platform=odroid-n2   # see Makefile for the full platform= list

make -C yabause/src/libretro clean
```

Debug/sanitizer builds:

```sh
make -C yabause/src/libretro platform=unix DEBUG=1
make -C yabause/src/libretro platform=unix DEBUG_ASAN=1
make -C yabause/src/libretro platform=unix DEBUG_UBSAN=1
make -C yabause/src/libretro platform=unix DEBUG_TSAN=1
```

ARM/AArch64 `platform=` targets set `DYNAREC_DEVMIYAX=1` (or `USE_ARM_DRC`/`USE_AARCH64_DRC`) to
build with the devMiyax SH-2 JIT instead of the plain interpreter; x86 `platform=unix` builds
interpreter-only unless a `USE_X86_DRC`-based target is used.

### Running

Needs a libretro frontend (RetroArch). BIOS goes in the frontend's system directory, named
`saturn_bios.bin`. Per `yabause/src/libretro/README.md`:

```sh
retroarch -L yabause/src/libretro/yabasanshiro_libretro.so path/to/game.iso
```

Known issue noted there: switching fullscreen↔windowed on the fly can crash the core.

### Testing

No automated unit-test suite is wired into the build — `test_framework.cpp`/`.h` exist under
`yabause/src/` but are commented out of `yabause/src/CMakeLists.txt`. Correctness is validated by
running real games/BIOS under RetroArch, plus `yabauseut/` — a separate hardware-conformance
test-ROM suite (needs an external `sh2eb-elf` Saturn cross-toolchain via the `iapetus` CMake
`ExternalProject`) that builds Saturn `.iso`s to run *in* the emulator and check against
real-hardware SH-2/VDP/SCU behavior. `yabause/src/tools/` (`cdtest.c`, `pertest.c`) are small
standalone diagnostic executables linked against `libyabause`, not a test suite.

## Architecture

### Layout

- `yabause/src/` — the core emulator (`libyabause`) and every frontend built on top of it.
  Frontend subdirs: `libretro/`, `qt/`, `gtk/`, `sdl/`, `glfw/`, `android/`, `ios/`, `cocoa/`,
  `nx/` (Switch), `dreamcast/`, `runner/`, `webinterface/`.
  Third-party/vendored: `musashi` and `c68k` (two interchangeable M68K cores, used for SCSP sound
  emulation — selected via `HAVE_MUSASHI`), `sh2_dynarec` and `sh2_dynarec_devmiyax` (two
  interchangeable SH-2 JIT backends, see below), `libchdr`, `libretro-common`, `json`, `nanovg`,
  `titan`, `aosdk`, `q68`.
- `yabauseut/` — hardware conformance test ROMs (see Testing above).
- `mini18n/` — vendored i18n library used by desktop frontends.
- `win_template/`, `snap/` — packaging.

### Core emulation loop

`YabauseEmulate()` in `yabause.c` is the heart of the emulator: a `while` loop that advances in
**decilines** (1/10 of a scanline), interleaving `SH2Exec()` for the Master and Slave SH-2 CPUs with
`ScuExec()` every deciline. Every 10th deciline it fires `Vdp2HBlankIN()`/`Vdp2HBlankOUT()` and
`ScspExec()` (sound). At `yabsys.LineCount == yabsys.VBlankLineCount` it fires `Vdp2VBlankIN()`; at
`yabsys.LineCount == yabsys.MaxLineCount` it fires `Vdp2VBlankOUT()` — which is where the actual
VDP1/VDP2 draw calls happen — and the frame ends. All of this runs on a single thread, in strict
interpreter-then-render order, driven by `MSH2`/`SSH2` cycle counts computed from
`yabsys.DecilineStop`/`DecilineUsec` (PAL vs. NTSC timing).

Key subsystems, each with an interpreter (always available) and sometimes a JIT:
- **SH-2** (`sh2int.c`/`.h`, `sh2core.c`, `sh2cache.c`): dual SH-2 CPUs (Master + Slave), both run
  inline on the main thread. Two dynarec backends exist as alternates to the interpreter —
  `sh2_dynarec` (older ARM-only DRC) and `sh2_dynarec_devmiyax` (`DYNAREC_DEVMIYAX=1`, used by most
  ARM/AArch64 `platform=` targets in the libretro Makefile) — selected via `SH2Core`.
- **SCU** (`scu.c`) + **SCU DSP**: DMA and the programmable DSP used for transform/physics math by
  many games, both interpreted inline via `ScuExec()`.
- **VDP1** (`vdp1.cpp`) command-list processing (drawing primitives) and **VDP2** (`vdp2.cpp`)
  background/scroll compositing — both draw via `vidogl.c`/`ygl*` (OpenGL) or `vidsoft.c`
  (software), invoked from `Vdp2VBlankOUT()`.
- **SCSP + M68K** (`scsp.c`/`scsp2.c` + `m68kmusashi.c`/`m68kc68k.c`): sound chip + its dedicated
  M68K CPU, driven synchronously from `YabauseEmulate()`'s per-deciline `ScspExec()` call.
- **CD block** (`cs0.c`/`cs1.c`/`cs2.c`, `cdbase.c`, platform `cd-*.c`) and **SMPC** (`smpc.c`,
  peripherals) round out the hardware model.
