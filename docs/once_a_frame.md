# Things being redone more often than once a frame

Follow-up to `docs/every_pixel.md`, for the CPU-reduction effort (target: ~32% → ~10% of one
core on the libretro/RetroArch interpreter build, see `CLAUDE.md`). Where `every_pixel.md`
catalogued *what runs per pixel*, this catalogues *what runs per pixel/texel/tile/scanline/
instruction/memory-access but is actually constant for the whole frame (or the whole VDP1/VDP2
draw pass)* — i.e. concrete hoisting targets, not just an inventory.

**Correction to `every_pixel.md`'s scope**: `yabause/src/libretro/libretro.c:983` hard-codes
`yinit.vidcoretype = VIDCORE_OGL` with no runtime switch. `vidsoft.c` and `titan/titan.c` — which
`every_pixel.md` covered heavily — are compiled into the libretro core but **never execute** on
this build. The two areas actually on the hot path for the profiled 32%-CPU run are:

1. **The active render path**: `vidogl.c` (GPU-accelerated renderer), `ygl_texture.cpp`,
   `yglcache.c`/`.cpp`/`ygles.c`, fed by `vdp1.cpp`/`vdp2.cpp`.
2. **The core CPU interpreter loop**: `sh2int.c` (SH-2 interpreter — confirmed active via
   `SH2CORE_INTERPRETER` in `libretro.c`), `sh2core.c`, `sh2cache.c`, `memory.c` (the memory bus
   every instruction goes through), `scu.c`, and `yabause.c`'s per-deciline loop.

Given the SH-2 interpreter executes on the order of ~477,000 cycles/frame *per CPU* (28.6MHz /
60fps), and most instructions are 1-2 cycles, per-instruction/per-memory-access overhead is
almost certainly the larger of the two — but both are covered below.

## Top findings overall, by estimated impact

1. **Idle/spin-loop detection exists (`sh2idle.c`) but is dead code** — unconditionally disabled
   by `#define EXEC_FROM_CACHE` at `sh2int.c:55`. Saturn games spend a lot of time in busy-wait
   polling loops (VBlank/HBlank/DMA-completion flags); this detector is built specifically to
   fast-forward those instead of interpreting them cycle-by-cycle, and it's currently off. Likely
   the single biggest win available. See "SH-2 interpreter dispatch" below.
2. **No cross-frame texture cache** — every VDP1 sprite texture and every VDP2 tile/bitmap
   texture is fully rebuilt from VRAM every frame, even for 100% static content, despite an
   already-existing content-addressed cache key that would make this straightforward to fix. See
   "Texture caching / dirty-tracking" below.
3. **The profiler (`profile.c`) is fully active in the shipping build** — `DONT_PROFILE` is never
   defined in the libretro Makefile, so `PROFILE_START`/`PROFILE_STOP` (linear `strcmp` tag
   lookup + `clock()`, several times per deciline × ~2630-3130 decilines/frame × 60fps) run for
   real, for zero benefit, in what should be a release build. Trivial one-line Makefile fix.
4. **Redundant double address-decode on every SH-2 memory access** — `GET_MEM_CYCLE_R`/`_W`
   (`memory.c`) and the actual read/write dispatch independently re-derive "which memory region
   is this address in" via two different mechanisms, on essentially every load/store instruction
   executed by both CPUs. See "Memory bus" below.
5. **`Vdp2GetAlpha`** re-reads `CCCTL` and duplicates work `info->blendmode` already encodes once
   per layer per frame, on every non-transparent VDP2 texel — and does genuinely nothing for the
   common case (`specialcolormode==0`). See "VDP2 texture/tile build" below.
6. **`RBGGenerator::update()`** unconditionally `memcpy`s the entire 512KB of VDP2 VRAM to the
   GPU every RBG-enabled frame, even though a complete, already-wired dirty-tracking mechanism
   (`A0_Updated`/`A1_Updated`/`B0_Updated`/`B1_Updated`) exists in the codebase to gate exactly
   this — its consumer branches are just empty. See "Texture caching / dirty-tracking" below.

---

## Render path (`vidogl.c` / `ygl_texture.cpp` / `yglcache` / `vdp1.cpp` / `vdp2.cpp`)

### Texture caching / dirty-tracking

#### No cross-frame texture atlas cache (vidogl.c:6099-6100, 4543-4544)
- **Current call frequency**: Once per frame, `VIDOGLVdp2DrawStart`/`VIDOGLVdp1DrawStart` call
  `YglTMReset`/`YglCacheReset` unconditionally, wiping the entire GPU texture atlas and its
  content-addressed hash table before every frame's draw pass.
- **What it computes**: `YglTMReset` rewinds the atlas allocator to (0,0); `YglCacheReset` clears
  the hash mapping content-key → atlas position. Every tile/bitmap/sprite texture is then
  re-decoded from VRAM and re-uploaded from scratch, every frame.
- **Why frame-constant / over-invalidated**: The cache key is already content-addressed, not
  frame-numbered — VDP2's `cacheaddr` packs `priority|alpha|paladdr|charaddr|transparencyenable|
  patternpixelwh|coloroffset` (vidogl.c:2551-2552); VDP1's key packs `CMDSRCA|CMDCOLR|CMDSIZE|
  CMDPMOD` (vidogl.c:4633-4637). Both uniquely identify "this exact tile/sprite with this exact
  palette/mode," which is stable across frames if the backing VRAM and relevant registers haven't
  changed. The cache today only dedups *within* one frame (e.g. one 8×8 char reused by 40 map
  cells decodes once) — it's thrown away before it could ever help across frames.
- **Hoisting proposal**: Add VRAM dirty-tracking (the choke points already exist — see next
  finding) and stop resetting the cache unconditionally each frame. Keep the atlas alive across
  frames; on a cache hit, additionally check whether the VRAM range backing `charaddr`/`paladdr`
  (and Color RAM, for palette modes) was written since the entry was cached, and only rebuild
  entries that were actually touched. The atlas allocator would need to become a real
  free-list/generation allocator instead of a per-frame bump allocator — that's the main added
  complexity.
- **Risk**: Medium-High for the full persistent cache (needs complete dirty-tracking proven to
  catch both CPU and SCU-DMA writes — confirmed both currently funnel through
  `Vdp1/Vdp2RamWrite*`, since `OPTIMIZED_DMA` — which would bypass those via a raw pointer,
  `scu.c:162-219` — is not defined anywhere in this tree; must be re-checked if that ever
  changes; needs partial-eviction support in the atlas; needs Color-RAM-dependent entries
  invalidated too). **Lower-risk first step (Low-Medium)**: VDP1-only, gated behind a single
  coarse "was `Vdp1Ram` touched since last frame" flag — already skips the reset for
  menus/static screens and games that build their sprite table once at startup.

#### Existing dead infrastructure for exactly this: `A0/A1/B0/B1_Updated` (vdp2.cpp:70-73, 147-227; vidshared.c:426-438; vidogl.c:7551-7554)
- **What exists already**: Four per-128KB-bank dirty flags, correctly **set** on every CPU/DMA
  write to VDP2 VRAM (`Vdp2RamWriteByte/Word/Long`, vdp2.cpp:158-226), correctly **reset** once
  per frame (`Vdp2DrawRBG0`, vidogl.c:7551-7554) — but their only **read** site
  (`vidshared.c:429-436`, RBG K-table setup) has four `if (A0_Updated == 1 && ...) { }` branches
  with **empty bodies**. The check runs; nothing happens. Looks like an abandoned optimization.
- **Hoisting proposal**: This is the natural gate for the `RBGGenerator::update()` finding right
  below — wire these already-correct flags up to actually skip work instead of doing nothing.
- **Risk**: Low to extend for the RBG-upload gate below (write-side already proven correct by
  every write path in this build). Medium if reused for the general texture-atlas cache above —
  128KB-bank granularity is too coarse to usefully skip individual tile rebuilds; would need a
  finer (e.g. 4KB-page) bitmap built on the same choke points first.

#### `RBGGenerator::update()` unconditional full-VRAM upload (ygl_texture.cpp:2509-2515)
- **Current call frequency**: Once per active RBG layer per frame (up to twice if both RBG0 and
  RBG1 are enabled).
- **What it computes**: `memcpy(mapped_vram, Vdp2Ram, 0x80000)` — copies all 512KB of VDP2 VRAM
  into a GPU SSBO for the RBG compute shader, unconditionally, every call.
- **Why over-invalidated**: `Vdp2Ram` only changes where CPU/DMA actually wrote it — most frames
  touch only a fraction (VDP1 texture pages, or one background's map). The dirty flags above are
  reset in the exact same call chain's caller, immediately before this runs, so they're perfectly
  positioned to gate it.
- **Hoisting proposal**: Skip the `memcpy` for banks whose flag is 0; skip the whole re-upload if
  none were written. Also skip the second RBG layer's upload in the same frame if the first
  already uploaded current data.
- **Risk**: Low — dirty flags already proven correct by every write path; safe to over-copy on a
  false positive (bank granularity), never under-copies (flags are OR'd, never cleared mid-frame).
- **Related, smaller**: The Color RAM upload two lines below (`glBufferSubData(...0x1000...)`,
  ygl_texture.cpp:2525) is also unconditional despite Color RAM already having *real*
  incremental dirty-range tracking elsewhere in the codebase (see next note) that isn't reused
  here — same fix, much smaller payoff (4KB vs 512KB).

#### Existing pattern worth reusing: Color RAM's incremental dirty-tracking (ygles.c:4355-4426, vdp2.cpp:269-350/1060/1889/2364)
Not a finding — a precedent. `YglOnUpdateColorRamWord` runs from every Color RAM write site,
incrementally updates a resolved-RGBA mirror buffer for just the written word, and tracks a
`[colupd_min_addr, colupd_max_addr]` dirty range that `YglUpdateColorRam` uses for a partial
`glTexSubImage2D` upload, gated by one `Vdp2ColorRamUpdated` flag. This is exactly the pattern
that should extend to VDP1/VDP2 VRAM for the texture-atlas cache and the RBG upload above — proof
it already works well in this codebase, just not applied to the much larger, much hotter VRAM path.

### VDP2 texture/tile build

#### `Vdp2GetAlpha` (vidogl.c:1919-1952)
- **Current call frequency**: Once per non-transparent output texel — every dot of every NBG0-3
  tile/bitmap layer in palette color modes.
- **What it computes**: Re-derives `CCMD` from `fixVdp2Regs->CCCTL` bit 8, then a switch on
  `info->specialcolormode` that for mode 0 changes nothing, for mode 1 depends only on a
  tile-constant, and only for modes 2/3 is genuinely per-pixel.
- **Why frame-constant**: `CCCTL` is written only at `vdp2.cpp:2225`, read here through
  `fixVdp2Regs` — a frame-frozen snapshot captured once at the top of `VIDOGLVdp2DrawStart`
  (vidogl.c:6076) and never reassigned during the draw pass. `Vdp2DrawNBG0` already computes
  `info->blendmode |= VDP2_CC_ADD` from the *identical* bit test (vidogl.c:6669) — provably the
  same frozen value, not just "usually" the same.
- **Hoisting proposal**: Replace the `CCCTL` read with `(info->blendmode & VDP2_CC_ADD) != 0`
  (macro already defined, ygl.h:315). Skip calling `Vdp2GetAlpha` entirely for
  `specialcolormode==0` (6 call sites) and use `info->alpha` directly — mirrors the
  `DoColorOffset`/`DoNothing` per-layer-constant dispatch pattern already used elsewhere in this
  file. For mode 1, hoist to the same per-tile granularity as `specialcolorfunction` itself
  (already computed once per tile in `Vdp2PatternAddrPos`).
- **Risk**: Low. `fixVdp2Regs` provably frozen for the whole pass; mode-0 skip is dead-code
  removal (the switch has no `case 0`). Doesn't interact with `Vdp2GeneratePerLineColorCalcuration`,
  which correctly handles the genuinely-per-scanline color-calc case separately.

#### `Vdp2DrawMapTest` missing tile/plane memoization present in its sibling `Vdp2DrawMapPerLine` (vidogl.c:3333-3438 vs. 2948-3163)
- **Current call frequency**: `Vdp2DrawMapTest` (ordinary NBG0/NBG1 tile drawing) calls
  `PlaneAddr`/`Vdp2PatternAddrPos` for every 8×8/16×16 tile, unconditionally.
- **Why this differs from a true "frame-constant" finding**: per-tile data genuinely varies
  (each tile can be a different character) — but its sibling `Vdp2DrawMapPerLine` (line-scroll/
  rotation path) already memoizes both calls with a "still in the same page/plane as last tile"
  guard (vidogl.c:3074-3096), and most consecutive tiles along a scanline *do* stay within the
  same 32×32/64×64-cell page. `Vdp2DrawMapTest` just doesn't have the same guard.
- **Hoisting proposal**: Port the `premapid`/`preplanex`/`prepagex`/`preplaney`/`prepagey` guard
  from `Vdp2DrawMapPerLine` into `Vdp2DrawMapTest`.
- **Risk**: Low — direct copy of an already-proven-correct pattern from the same file.

### VDP1 texture build

#### `Vdp1ReadTexture`'s repeated `fixVdp2Regs->SPCTL` reads (vidogl.c:564-928)
- **Current call frequency**: `SPCTL & 0x20` (RGB-sprite-permission) tested 8 separate times
  across the six per-texel color-mode loops; `SPCTL & 0xF` (sprite type) re-passed fresh 4 more
  times — i.e. effectively once per texel of every sprite drawn that frame.
- **Why frame-constant** (stronger — *whole-command-list-pass-constant*): `SPCTL` is written only
  at `vdp2.cpp:2207`. `VIDOGLVdp1DrawStart` takes a private snapshot —
  `fixVdp2Regs = Vdp2RestoreRegs(...); memcpy(&baseVdp2Regs, fixVdp2Regs, sizeof(Vdp2));
  fixVdp2Regs = &baseVdp2Regs;` (vidogl.c:4526-4529) — before processing the *entire* VDP1
  command list against that one frozen copy. Nothing reassigns it until the pass ends. Provably
  identical across every texel of every sprite in the frame.
- **Hoisting proposal**: Hoist to two locals at the top of `Vdp1ReadTexture` (the same place
  `SPCCCS` is already correctly hoisted, vidogl.c:584): `rgbSpritesAllowed` and `spriteType`.
  Could go one level further to a true per-frame global in `VIDOGLVdp1DrawStart`, but
  per-command is already most of the win with less risk.
- **Risk**: Low — mechanical dedup against a struct provably frozen for the whole pass.

### Per-scanline setup

#### Dead per-scanline VRAM read: `Vdp2HBlankOUT`'s `cell_scroll_data` fill (vdp2.cpp:786-789)
- **Current call frequency**: Once per scanline, unconditionally — `Vdp2HBlankOUT()` runs every
  time `DecilineCount` reaches 10 in `yabause.c`'s main loop, up to ~262-263×/frame (NTSC).
- **What it computes**: 88 VRAM long-reads per scanline (~23,000/frame) into `cell_scroll_data`,
  regardless of whether anything uses vertical cell scroll.
- **Why it's dead, not just hoistable**: `cell_scroll_data` is consumed only by `vidsoft.c`'s
  draw entry points. Grep confirms `vidogl.c`/`ygles.c`/`ygl_texture.cpp` have zero references —
  `vidogl.c`'s vertical-scroll handling reads `info->verticalscrolltbl` directly instead. Since
  `vidsoft.c` never executes on this build (`VIDCORE_OGL` hardcoded), this loop computes a table
  with zero consumers, every scanline, forever. (The `memcpy` of `Vdp2Regs` on the line above it
  *is* needed — only the `cell_scroll_data` loop is dead.)
- **Hoisting proposal**: Gate behind `VIDCore->id == VIDCORE_SOFT`, or `#if 0` it with a comment,
  since this build only ever compiles/links the libretro OGL path.
- **Risk**: Low — single-consumer dead work, confirmed by grep; fix is a one-line branch.

#### Confirmed *not* findings (existing correct per-line patterns)
- `Vdp2DrawMapPerLineNbg23`'s per-scanline `Vdp2RestoreRegs` call — genuine per-line raster
  "line scroll" hardware feature, and the call itself is trivial pointer arithmetic.
- `Vdp2GeneratePerLineColorCalcuration` — named "PerLine" but only called once per background per
  frame; internally only walks a per-scanline loop when a per-frame diff check
  (`Vdp2HBlankOUT`, vdp2.cpp:791-856) already determined this layer's color-calc actually varies
  within the frame. Already well-optimized.

---

## Core CPU interpreter loop

### SH-2 interpreter dispatch

#### Idle/spin-loop detection disabled by `EXEC_FROM_CACHE` (sh2int.c:55, 3156-3161; all of sh2idle.c)
- **Current call frequency**: `SH2InterpreterExec` is the live dispatch loop (confirmed active
  core via `libretro.c:66,740`); the `#ifndef EXEC_FROM_CACHE` block that would call
  `SH2idleCheck`/`SH2idleParse` never compiles in because `EXEC_FROM_CACHE` is unconditionally
  `#define`d — every instruction, idle or not, runs through the plain per-instruction loop.
- **What it does**: `sh2idle.c` pattern-matches short backward-branch loops (`bt`/`bf`/`bts`/
  `bfs`), tracks whether registers become deterministic across one iteration, and — if the loop
  provably doesn't touch memory or depend on iteration count — sets `isIdle=1` and fast-forwards
  `cycles` straight to the target instead of re-executing cycle-by-cycle.
- **Why wasteful**: Git history shows idle detection used to be the default; a later, unrelated
  feature (executing from the SH-2 cache data array at `0xC0000000`, a niche compatibility hack)
  got bolted onto the same `#ifdef` guard and ended up unconditionally on in this fork, silently
  disabling idle detection as a side effect. Spin-wait loops (poll VBlank/HBlank/DMA/SCU status)
  are extremely common in Saturn game code and are exactly what this detector targets.
- **Hoisting proposal**: Decouple the two concerns — keep the cheap `0xC0000000` DataArray check
  always available, re-enable the idle-loop-skip path independently.
- **Risk**: Medium — directly manipulates `context->cycles` (skips cycles), so misdetection has
  a direct A/V-sync/timing consequence, and the mechanism hasn't been exercised in this codebase
  for a long time. The detector is deliberately conservative (bails on any memory write or
  non-deterministic register use) and is previously-shipped code, not new logic — worth serious
  testing given the likely payoff, but test broadly (games using tight DMA-synced timing loops
  are the failure mode to watch for).

#### Instruction fetch — no "last page" cache (sh2int.c:3171, fetchlist[0x100] at sh2int.c:174)
- **Current call frequency**: Once per instruction fetch — hundreds of thousands of times/frame
  per CPU: `fetchlist[(PC >> 20) & 0x0FF](PC)`, an indirect call through a 256-entry table keyed
  by address top byte.
- **Why hoistable**: The region a given PC page maps to never changes at runtime, and code has
  strong locality — most consecutive fetches stay on the same 1MB page. Every fetch still pays a
  full indirect call regardless.
- **Hoisting proposal**: Add a tiny "last page" hint cache (remember last page's base
  pointer/mask + page number); on a match, do a raw masked array read instead of the indirect
  call, falling back to `fetchlist[]` only on a page miss. Self-modifying code is unaffected —
  only the region *classification* is cached, actual bytes are still read fresh.
- **Risk**: Medium — hottest path in the emulator; needs real benchmarking, since modern branch
  predictors already handle a stable indirect-call target well, so the win is less certain than
  it looks on paper. Prototype and measure before committing.

#### `SH2sleep` doesn't fast-forward cycles (sh2int.c:2632-2635)
- **Current call frequency**: Once per SLEEP execution; if halted for N cycles awaiting an
  interrupt, called ~N/3 times (interrupts are only serviced at the top of `SH2InterpreterExec`,
  not inside this loop).
- **Why wasteful**: SLEEP guarantees no state change until an interrupt, and interrupts aren't
  checked until the next `SH2Exec` call boundary anyway — no reason to loop-and-refetch.
- **Hoisting proposal**: Special-case SLEEP in the dispatch loop: set `cycles = target_cycle` and
  break, instead of looping.
- **Risk**: Low — behavior is identical either way; this just stops computing "nothing happens"
  the slow way.

#### Profiler active in the shipping build (`profile.c`/`.h`; yabause.c:725-858)
- **Current call frequency**: ~5 unconditional `PROFILE_START`/`STOP` pairs per deciline
  ("Total Emulation", "SCU", "68K", "SMPC", "CDB") × ~2630-3130 decilines/frame × 60fps ≈ >1.5M
  calls/sec, plus more conditional ones per scanline.
- **What it does**: Each call does a linear `strcmp`-based tag lookup (`ProfileStart` does a
  second linear scan via `Nested()`) plus a `clock()` call.
- **Why wasteful**: `profile.h:20` already gates the whole feature behind `#ifdef DONT_PROFILE`
  — but `DONT_PROFILE` is defined nowhere in the tree, so the libretro release build ships with
  profiling fully active for no reason.
- **Hoisting proposal**: Define `DONT_PROFILE` in the libretro Makefile/CFLAGS.
- **Risk**: Low — strips a diagnostic feature that already has its own opt-out, just not wired
  into this build. No behavioral/timing impact.

### Memory bus

#### Redundant double address-decode per access (memory.c:734-798 `GET_MEM_CYCLE_R`/`_W`, plus the separate region `switch` + page-table dispatch at memory.c:857-991)
- **Current call frequency**: Every SH-2 data memory read/write from the interpreter — a large
  fraction of executed instructions.
- **What it does**: `GET_MEM_CYCLE_*` runs a sparse `switch (addr & 0xDFF00000)` (too sparse to
  jump-table, compiles to a compare chain) purely to compute wait-state cycles; a *separate*
  `switch(addr >> 29)` + `ReadByteList/WriteByteList[(addr>>16)&0xFFF]` table does the actual
  dispatch. Two independent classifications of the same address, on every access.
- **Hoisting proposal**: Add parallel `ReadCycleList[0x1000]`/`WriteCycleList[0x1000]` arrays
  populated at `MappedMemoryInit()` time alongside `ReadByteList`/`WriteByteList`, indexed
  identically. Cost lookup becomes a single array read for every region except VDP2 RAM (whose
  cost is genuinely dynamic — depends on `yabsys.LineCount`/`Vdp2External.cpu_cycle_a/b`); keep
  the existing `getVramCycle()` call for just that handler, since VDP2 RAM is a small fraction of
  total access traffic vs. LWRAM/HWRAM.
- **Risk**: Medium — cycle-timing-sensitive by definition (the whole point of this code is A/V
  sync accuracy); the replacement table must reproduce the exact same per-region constants
  (verified against memory.c:749-798) and the VDP2 dynamic path must stay intact. Functionally
  equivalent if done carefully, but a table-population mistake would silently desync timing
  rather than crash — needs a cycle-count regression test against the current build.

#### `cycle` output-parameter convention adds overhead vs. accumulating in-place (memory.c:850/914/977/1043/1107/1171, ~40 call sites in sh2int.c)
- **What it does**: `MappedMemoryReadByte(addr, &rcycle)` writes cost out through a pointer; the
  caller then does `sh->cycles += 1 + rcycle`. Needs a stack slot, a store through the pointer,
  and a load-and-add back — extra traffic a direct accumulate wouldn't need.
- **Hoisting proposal**: Since `CurrentSH2` is already a live global (set once at the top of
  `SH2Exec`, already relied on elsewhere, e.g. `FetchBios`), have the accessors do
  `CurrentSH2->cycles += cost` internally and return only the data value, dropping the pointer
  param and the local at each of the ~40 call sites.
- **Risk**: Medium — mechanical but touches every memory-instruction handler; must preserve the
  existing "+1 fixed / +cycle wait-state" split exactly per instruction (some add `1+rcycle`,
  some just `rcycle` — intentional, timing-derived differences that must be preserved). Careful
  mechanical refactor with diff review, not a blind find-and-replace.

### SH-2 cache

#### Confirmed already off, no action needed (memory.h:50; sh2cache.c; guarded sites in sh2int.c/memory.c)
`CACHE_ENABLE` is commented out in the source and defined nowhere in the tree — every `#if
CACHE_ENABLE` guard already compiles to the plain non-cache path. Cache emulation is *not* on the
hot path today; this item from the original investigation brief is already satisfied. Optional
hygiene only: `sh2cache.c` could be excluded from the libretro build entirely (binary size, not
perf), and it's worth confirming no other build target silently relies on `CACHE_ENABLE` being
defined externally.

### SCU / main loop

#### SCU DSP breakpoint scan runs even with zero breakpoints (scu.c:1370-1376)
- **Current call frequency**: Once per DSP cycle while the DSP is running (game/feature
  dependent, not universal).
- **Why minor**: Zero-iteration loop when `numcodebreakpoints==0` (the normal case) — cheap, but
  still a load+compare+branch per DSP instruction for an inactive debug feature.
- **Hoisting proposal**: Guard the whole loop behind a single `breakpointEnabled`-style flag.
- **Risk**: Low. Small — DSP-active time is a minority of frame time for most titles; low
  priority relative to the SH-2/memory findings above.

#### Confirmed *not* findings
- `ScuDmaProc`/`SucDmaCheck` — called 3×/deciline but each is a single early-out `if` when no
  DMA is in flight; negligible.
- `use_new_scsp`/`sync_shift` checks in `YabauseEmulate` — effectively frame-constant but cost is
  one branch each, ~2630-3130×/frame; not worth restructuring on its own next to the findings above.

---

## Notes for whoever implements these

- The dirty-tracking findings (texture cache, RBG upload) all rest on one verified fact: SCU DMA
  in this build has no bypass around `Vdp1/Vdp2RamWrite*` (`OPTIMIZED_DMA` is undefined
  everywhere in the tree). Re-verify this if `OPTIMIZED_DMA` is ever turned on for a DMA-side
  performance push — it would silently break any dirty-tracking built only on those write
  functions.
- Two of the highest-risk items (idle-loop detection, the memory-bus cycle-table merge) are also
  the two most likely to actually matter for the 32%→10% target, since both sit on the
  per-instruction/per-access hot path that dwarfs per-frame or per-scanline costs by sheer call
  count. Per the project's stated approach (`CLAUDE.md`): no need to be precious about it — try
  the change, run the game, compare `ps -o %cpu` against the baseline, and revert with git if it
  breaks something rather than trying to prove safety in advance.
- The trivial ones (`DONT_PROFILE`, `SH2sleep` fast-forward, `Vdp2DrawMapTest` memoization) cost
  almost nothing to try first and derisk nothing about the bigger changes — worth doing first for
  quick, safe wins and to sanity-check the `ps -o %cpu` measurement methodology itself.
