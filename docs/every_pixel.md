# Functions called once per pixel

An inventory of every function in the renderer code (`yabause/src/vidsoft.c`, `vidshared.c`,
`vidogl.c`, `titan/titan.c`) that is invoked from inside a genuine **per-pixel loop** — an
inner-most loop stepping over individual output pixels or source texels — rather than once per
scanline or once per frame. For each function: what it does, where it's called from, *why* it
runs per pixel on real hardware (or why the current code happens to call it per pixel), and
whether that granularity is a real hardware requirement or could plausibly be hoisted to a
coarser granularity.

Two independent renderer backends exist and are covered separately, since they solve the
per-pixel problem very differently:

- **`vidsoft.c`** — the pure-software renderer. It has no GPU to offload to, so it does genuinely
  everything (texture fetch, window test, color-calc, compositing) per pixel on the CPU, and
  relies on manual memoization (see "Existing per-line/per-cell hoisting" below) to keep that
  affordable.
- **`vidogl.c`** — the OpenGL-accelerated renderer. It still has to do per-pixel *format decode*
  work on the CPU when building textures from Saturn VRAM (palette lookups, special-priority/
  color-calc bits are baked into the texture data), but it pushes window testing, mosaic, color
  offset, and (where a compute shader is available) even rotation-background sampling onto the
  GPU — those are called out explicitly below as *not* per-pixel CPU work, since it would be easy
  to assume otherwise.

`titan.c`, the priority-sort/blend compositor, is wired only into `vidsoft.c` — `vidogl.c` never
calls it and instead lets OpenGL's own blend/depth state composite layers. It's included here
under its own heading because it does real per-pixel CPU work, but note it's not part of the GPU
render path.

---

## `vidsoft.c` — software renderer

Two families of per-pixel loops exist here:
- **VDP1**: `iterateOverLine()` (vidsoft.c:2831), the Bresenham line stepper used by every
  VDP1 sprite/polygon/line/polyline draw. Its callback `DrawLineCallback` (vidsoft.c:2923) runs
  once per output dot.
- **VDP2**: each background layer has an outer per-scanline loop and an inner per-pixel loop —
  `Vdp2DrawScroll` (loop at vidsoft.c:1005), `Vdp2DrawRotationFP` (loops at vidsoft.c:1183 and
  vidsoft.c:1323, no-coefficient and coefficient paths respectively), and `VidsoftDrawSprite`
  (loop at vidsoft.c:3637).

### VDP1

#### `DrawLineCallback` (vidsoft.c:2923)
- **What**: Per-dot callback of the Bresenham stepper; advances Gouraud color accumulators,
  samples the texture, writes the pixel.
- **Called from**: `iterateOverLine` (vidsoft.c:2849-2905), itself called by `DrawLine`
  (vidsoft.c:2949) from `drawQuad`'s per-row loop (vidsoft.c:3133).
- **Why per-pixel**: Real VDP1 hardware rasterizes with arbitrary diagonal Bresenham stepping,
  not horizontal scanlines (see the comment at vidsoft.c:123-126, which notes this is *why*
  VDP1 half-transparency produces moiré patterns on real hardware). Each dot needs its own
  texture coordinate and Gouraud-interpolated color.
- **Hoistable?**: No — this is the fundamental VDP1 draw primitive.

#### `getpixel` (vidsoft.c:2518)
- **What**: Texture sample — reads the pattern data for the current line/column, resolves
  palette/color-bank lookup, detects end-codes.
- **Called from**: `DrawLineCallback` (vidsoft.c:2933).
- **Why per-pixel**: VDP1 texture X coordinate advances every dot; each dot is a genuinely
  distinct VRAM read.
- **Hoistable?**: No.

#### `Vdp1ReadPattern16/64/128/256/64k` (vidsoft.c:2460, 2468, 2473, 2478, 2483)
- **What**: Raw VRAM byte/word fetch + bit-depth mask for the 5 VDP1 color modes.
- **Called from**: `getpixel` (vidsoft.c:2565-2619).
- **Why per-pixel**: Texel offset changes every dot.
- **Hoistable?**: No (trivial helper, but genuinely per-dot).

#### `putpixel` (vidsoft.c:2742) / `putpixel8` (vidsoft.c:2707)
- **What**: Writes one VDP1 framebuffer pixel — mesh dithering, clip test, interlace-skip test,
  CMDPMOD blend mode (replace/shadow/half-luminance/half-transparent/Gouraud).
- **Called from**: `DrawLineCallback` (vidsoft.c:2939/2941).
- **Why per-pixel**: Mesh checkerboard test, clip test, and Gouraud/half-transparent blending
  against the current framebuffer content are genuinely per-dot on hardware.
- **Hoistable?**: Partially:
  - The `putpixel` vs `putpixel8` **selection** is driven by `vdp1pixelsize`, fixed once per
    VDP1 draw-start (vidsoft.c:2407/2414) — yet `DrawLineCallback` re-tests it
    (`vdp1pixelsize == 2`) on every single dot (vidsoft.c:2938). Picking a callback/putpixel
    function pointer once per command instead would remove a per-pixel branch.
  - `CheckDil`'s result (below) doesn't depend on `x` at all and is being recomputed every dot
    for no reason.

#### `CheckDil` (vidsoft.c:2653)
- **What**: Interlace field-skip test.
- **Called from**: `putpixel` (vidsoft.c:2749), `putpixel8` (vidsoft.c:2717).
- **Why it currently runs per-pixel**: Only because the call site is inside `putpixel`/`putpixel8`.
- **Hoistable? Yes.** Depends only on `y` and frame-constant regs (`regs->FBCR`,
  `vdp1interlace`) — none of which vary across a scanline. It recomputes an identical boolean
  for every dot on the same line. Could be evaluated once per row (or the whole row skipped
  up-front) instead.

#### `IsClipped` (vidsoft.c:2690) / `IsUserClipped` (vidsoft.c:2674) / `IsSystemClipped` (vidsoft.c:2682)
- **What**: User-clip / system-clip rectangle test.
- **Called from**: `putpixel` (vidsoft.c:2761), `putpixel8` (vidsoft.c:2726).
- **Why per-pixel**: The clip rectangle is a per-command constant, but `x`/`y` vary every dot
  along the Bresenham line, so as structured the test must run per-dot.
- **Hoistable? Maybe.** The rectangle itself could be used to clip the line's start/end
  coordinates *before* rasterizing (Cohen-Sutherland-style) instead of testing membership on
  every dot — would avoid touching pixels outside the rect at all on large off-screen sprites.

#### `alphablend16` (vidsoft.c:2490)
- **What**: 15-bit RGB alpha blend between destination and source colors.
- **Called from**: `putpixel` (vidsoft.c:2782, 2789, 2825).
- **Why per-pixel**: Blends against whatever is already in the framebuffer at that exact dot,
  which is arbitrary per pixel — genuine per-dot compositing.
- **Hoistable?**: No.

#### `gouraudAdjust` (vidsoft.c:2642)
- **What**: Clamped add of a per-vertex-interpolated Gouraud delta to one color channel.
- **Called from**: `putpixel`, Gouraud case (vidsoft.c:2811/2815/2819).
- **Why per-pixel**: The Gouraud accumulator is stepped every dot in `DrawLineCallback`
  (vidsoft.c:2928-2930) — this *is* what Gouraud shading means.
- **Hoistable?**: No (trivial function, genuinely per-pixel work).

### VDP2

#### `Vdp2FetchPixel` (vidsoft.c:385)
- **What**: Fetches one background-layer texel (4/8/16bpp-palette/16bpp-RGB/32bpp) and, for
  palette modes, resolves it through color RAM.
- **Called from**: `Vdp2DrawScroll` (vidsoft.c:1047), `Vdp2DrawRotationFP`
  (vidsoft.c:1201/1412) — all inside their per-pixel loops.
- **Why per-pixel**: The character/pattern address inside the tile is recomputed every dot by
  scroll/zoom/rotation math — the source texel genuinely changes every dot.
- **Hoistable?**: No for the texel value itself.

#### `Vdp2ColorRamGetColorSoft` (vidsoft.c:206)
- **What**: Converts a palette/color-RAM index into RGB(+MSB).
- **Called from**: `Vdp2FetchPixel` (vidsoft.c:395/403/411).
- **Why per-pixel**: The palette index is per-pixel data (character data varies every dot).
- **Hoistable?**: No.

#### `Vdp2MapCalcXY` (vidsoft.c:571)
- **What**: Converts a scroll-screen (x,y) into tile/plane/page addressing, incl. flip logic.
- **Called from**: `Vdp2DrawScroll` (vidsoft.c:1033), `Vdp2DrawRotationFP`
  (vidsoft.c:1197/1377/1407).
- **Why per-pixel**: Tile addressing is a function of the current (x,y), which changes every
  dot under scroll/zoom/rotation.
- **Hoistable? Already hoisted.** Contains an explicit memoization check
  (`if (check != sinfo->oldcellcheck)`, vidsoft.c:581) so the expensive plane/page/pattern
  address recompute (`Vdp2PatternAddr`) only runs when the 8×8/16×16 cell actually changes, not
  every dot — a real existing optimization, worth preserving as a pattern.

#### `Vdp2PatternAddr` (vidsoft.c:239)
- **What**: Decodes pattern-name-data (character number, palette bank, flip bits).
- **Called from**: `Vdp2MapCalcXY` (vidsoft.c:601) — only executes when the memoized cell
  check fails. The code has an inline comment flagging it: *"Heh, this could be optimized"*
  (vidsoft.c:601).
- **Hoistable?**: Effectively already hoisted to per-cell granularity via `Vdp2MapCalcXY`'s
  memoization; not truly per-pixel in practice.

#### `TestBothWindow` (vidsoft.c:508) / `TestWindow` (vidsoft.c:437) / `TestSpriteWindow` (vidsoft.c:466) / `WindowLogic` (vidsoft.c:496)
- **What**: VDP2 window (clip) logic — window 0/1 + sprite-window combined per WCTL.
- **Called from**: `Vdp2DrawScroll` (vidsoft.c:1014/1076), `Vdp2DrawRotationFP`
  (vidsoft.c:1187/1346/1349/1367), `VidsoftDrawSprite` (vidsoft.c:3645/3693/3722/3780/3811).
- **Why per-pixel**: `TestSpriteWindow` genuinely needs per-pixel granularity — the sprite
  window mask is built from VDP1's actual rendered framebuffer (real hardware feature:
  MSB-shadow pixels punching per-dot holes in the sprite window). Ordinary window 0/1 use a
  clip rectangle that's constant per scanline (updated once per line by
  `ReadLineWindowClip`) — only the `x >= xstart && x <= xend` comparison genuinely needs `x`.
- **Hoistable? Maybe.** Restructuring the loop to jump straight to `[xstart,xend]` instead of
  testing membership across the whole scanline would produce identical results with less
  per-pixel branching, especially on hi-res (704-wide) scanlines.

#### `GetAlpha` (vidsoft.c:736)
- **What**: Alpha/color-calc-enable value for a pixel, from VDP2 special-color-calc mode and
  the pixel's own color index/MSB.
- **Called from**: `Vdp2DrawScroll` (vidsoft.c:1079), `Rbg0PutPixel`/`Rbg0PutHiresPixel`.
- **Why per-pixel**: Special color-calc modes 2/3 genuinely depend on the fetched pixel's own
  color-index bits/MSB — real per-dot hardware behavior.
- **Hoistable?**: No for modes 2/3.

#### `PixelIsSpecialPriority` (vidsoft.c:755)
- **What**: Tests whether the pixel's color index falls in one of 8 special-priority ranges.
- **Called from**: `Vdp2DrawScroll` (vidsoft.c:1061), only when `specialprimode == 2`.
- **Why per-pixel**: Genuine hardware feature — per-pixel priority selection keyed on that
  pixel's own color code.
- **Hoistable?**: No.

#### `DoColorOffset` (vidsoft.c:331) / `DoNothing` (vidsoft.c:324)
- **What**: Applies (or no-ops) the VDP2 color-offset RGB add.
- **Called from**: `Vdp2DrawScroll`, `Rbg0PutPixel`/`Rbg0PutHiresPixel`, `VidsoftDrawSprite`.
- **Why per-pixel**: The operands (`cor/cog/cob`) and the function-pointer selection are
  already hoisted to once-per-line (`ReadVdp2ColorOffset`/`LoadLineParamsNBGx`); only the
  trivial per-pixel add itself remains, which is inherent (each pixel's color differs).
- **Hoistable?**: Already well-hoisted.

#### `Rbg0PutPixel` (vidsoft.c:1100) / `Rbg0PutHiresPixel` (vidsoft.c:1090)
- **What**: Applies color-offset/alpha and writes an RBG0/RBG1 pixel to the compositor
  (`TitanPutPixel`), doubling horizontally in hi-res mode.
- **Called from**: `Vdp2DrawRotationFP` (vidsoft.c:1206/1417).
- **Why per-pixel**: Each dot has its own color and must be individually composited.
- **Hoistable?**: No (trivial wrapper around an inherently per-dot write).

#### `GenerateRotatedXPosFP` / `GenerateRotatedYPosFP` (vidshared.h:338/347)
- **What**: Applies the RBG rotation matrix to a screen x-coordinate to get the source (x,y).
- **Called from**: `Vdp2DrawRotationFP` — no-coefficient path (vidsoft.c:1190/1191) and
  coefficient path (vidsoft.c:1353/1354, 1383/1384).
- **Why per-pixel**: Genuinely varies every dot when a per-pixel coefficient table with
  `deltaKAx != 0` is active.
- **Hoistable? Yes, partially — a real finding.** `Xsp = A*xmul + B*ymul + C`
  (vidshared.h:340) does **not** depend on `x`; only the trailing `+ mulfixed(dX, tofixed(x))`
  term does, and the result is then scaled by `kx`. When coefficients are disabled, or when
  `deltaKAx == 0` for the scanline (`kx`/`ky` scanline-constant), the whole per-pixel result is
  an **affine function of `i`** (`result = base + step*i`). The code fully recomputes the
  fixed-point multiply chain from scratch every pixel in this case, even though `A,B,C,xmul,
  ymul,kx,dX` are all scanline-constant — could be hoisted to a per-scanline base plus a
  per-pixel incremental add, removing ~4 fixed-point multiplies per pixel in the common case.
  (When `deltaKAx != 0`, per-pixel recomputation is a genuine hardware requirement — see next
  entry.)

#### `Vdp2ReadCoefficientFP` (vidshared.h:784) / `Vdp2ReadCoefficientMode0_2FP` (vidshared.c:765)
- **What**: Reads one entry from the VDP2 rotation coefficient table in VRAM for the current dot.
- **Called from**: `Vdp2DrawRotationFP` (vidsoft.c:1329/1338) — only when `p->deltaKAx != 0`.
- **Why per-pixel**: The coefficient table exists specifically so `kx`/`ky` can change every
  dot (e.g. Sega Rally-style road curvature) — a genuine, unavoidable hardware feature.
- **Hoistable?**: No when `deltaKAx != 0`. Already correctly hoisted to once-per-scanline
  (vidsoft.c:1294-1307) for the `deltaKAx == 0` case — a good existing optimization.

#### `TestSpriteWindow`'s mask read: `sprite_window_mask[(y*vdp2width)+x]` (vidsoft.c:474)
- **What**: Reads the per-dot sprite-window mask, filled by `VidsoftDrawSprite` from VDP1's
  MSB-shadow output.
- **Called from**: `TestBothWindow` (vidsoft.c:512).
- **Why per-pixel**: The mask is inherently per-dot data (built from VDP1's rendered
  framebuffer) — genuine hardware behavior.
- **Hoistable?**: No.

#### `VidsoftDrawSprite` inline per-pixel work (loop at vidsoft.c:3637)
Most sprite color-calc/shadow logic is inlined directly in the loop body
(vidsoft.c:3637-3853), but it calls, per pixel:
- **`Vdp1GetSpritePixelInfo`** (vidshared.h:829, vidsoft.c:3712/3801) — decodes VDP1
  sprite-type priority/color-calc/shadow bits from the raw VDP1 framebuffer pixel. Reads the
  actual VDP1 output at (x,y), which is arbitrary per dot — a real hardware pipeline stage. Not
  hoistable.
- **`Vdp2ColorRamGetColor`** (defined in vidogl.c, declared ygl.h:850, vidsoft.c:3720/3809) —
  color-RAM lookup for the VDP1-framebuffer color-bank pixel. Genuine per-pixel palette lookup.
  Not hoistable.
- The **framebuffer-readout-position computation** (vidsoft.c:3651-3680) branches on the
  VDP1/VDP2 resolution combination *every pixel*, even though the branch outcome
  (`vdp1width==1024 && vdp2_x_hires`, etc.) is frame-constant. **Hoistable** — selecting the
  stepping mode once per frame instead of re-testing 4 resolution-combination conditions on up
  to 704×512 pixels would remove a per-pixel branch.

### Existing per-line/per-cell hoisting already present in `vidsoft.c` (for contrast)

Worth preserving as reference patterns when addressing the "hoistable" items above:
- `Vdp2MapCalcXY`'s cell memoization (`oldcellcheck`, vidsoft.c:581).
- `Vdp2ReadCoefficientFP`'s scanline fast path when `deltaKAx == 0` (vidsoft.c:1294-1307).
- The precomputed `mosaic_table[16][1024]` (vidsoft.c:864-877) — turns per-pixel mosaic
  division into an O(1) table lookup.
- `DoColorOffset`/`DoNothing` function-pointer selection and `cor/cog/cob` operands, hoisted to
  once-per-line in `ReadVdp2ColorOffset`/`LoadLineParamsNBGx`.

---

## `vidogl.c` — OpenGL-accelerated renderer

This renderer still does per-*texel* CPU work when converting Saturn VRAM data into textures
(palette lookups and special-priority/color-calc bits are baked into the texture data before
upload), but window testing, mosaic, and color-offset are all done on the GPU. Where relevant
below this is called out explicitly, since a per-pixel CPU loop would be easy to assume but
isn't actually there.

### VDP1

#### `Vdp1ReadTexture` (vidogl.c:564)
- **What**: Converts a VDP1 sprite's raw VRAM texel data (4bpp bank/LUT, 8bpp 64/128/256-color
  bank, 16bpp RGB) into RGBA8 texture data — per texel, including end-code/transparent-pixel
  detection and shadow/priority tagging. This function *is* the per-texel loop itself
  (vidogl.c:611-923), not a sub-call from one.
- **Called from**: `VIDOGLVdp1NormalSpriteDraw`/`ScaledSpriteDraw`/`DistortedSpriteDraw`
  (vidogl.c:4696, 4714, 4910, 4930, 5164, 5176), once per sprite command.
- **Why per-pixel**: Each texel byte/word packs 1-2 independent color indices/RGB values, each
  independently checked against SPD/end-code/MSB-shadow rules — genuinely per-texel data.
- **Hoistable?**: No for the base conversion. The six color-mode branches duplicate
  near-identical end-code/SPD/shadow logic — worth de-duplicating for maintainability, but that
  wouldn't change the per-pixel cost.

#### `Vdp1MaskSpritePixel` (vidogl.c:269)
- **What**: Extracts the embedded 3-bit color-calc-ratio field from a raw pixel value (16bpp/
  8bpp-256/8bpp-128 modes; bit position depends on `SPCTL` sprite type).
- **Called from**: `Vdp1ReadTexture` (vidogl.c:872, 916).
- **Why per-pixel**: The ratio bits are packed into each individual texel's value on real
  hardware — no per-row/per-sprite constant to hoist to.
- **Hoistable?**: No.

#### `Vdp1ProcessSpritePixel` (vidshared.h:985) / `Vdp1GetSpritePixelInfo` (vidshared.h:829)
- **What**: Decodes a 4bpp-LUT sprite texel's color-RAM word into shadow/priority/color-calc-ratio.
- **Called from**: `Vdp1ReadTexture` (vidogl.c:461, 708, 747), per texel in LUT mode.
- **Why per-pixel**: In LUT mode each texel indexes an independent color-RAM entry whose
  MSB/priority bits can differ texel to texel.
- **Hoistable?**: No.
- **Note**: also called once (non-per-pixel) from `VIDOGLVdp1PolygonDraw`/`PolylineDraw`/
  `LineDraw` for their single flat-fill color — VDP1 flat polygons only ever resolve one color,
  so those call sites are excluded from this inventory.

### VDP2

#### `Vdp2GetPixel4bpp` (vidogl.c:1955) / `Vdp2GetPixel8bpp` (vidogl.c:2011) / `Vdp2GetPixel16bpp` (vidogl.c:2040)
- **What**: Reads VRAM word(s), unpacks palette-indexed dot(s), writes coloroffset|paladdr|dot
  plus special-priority/alpha bits into the texture buffer.
- **Called from**: `Vdp2DrawBitmap`, `Vdp2DrawCell`, `Vdp2DrawBitmapLineScroll`,
  `Vdp2DrawBitmapCoordinateInc` — all inside their per-texel `j` loops.
- **Why per-pixel**: Each dot can independently be transparent and has its own
  special-priority/alpha evaluation depending on its raw value.
- **Hoistable?**: No — this is the core palette-index decode; a GPU shader could do it instead
  (as the RBG path does, see below), but as CPU code it can't be coarsened.

#### `Vdp2GetPixel16bppbmp` (vidogl.c:2053) / `Vdp2GetPixel32bppbmp` (vidogl.c:2061)
- **What**: Reads a direct-RGB (non-palette) bitmap pixel and applies the `SAT2YAB1/2`
  alpha-tag macro; transparency test on the RGB MSB.
- **Called from**: same bitmap/cell/line-scroll functions as above.
- **Why per-pixel**: Each pixel is an independent direct-color sample.
- **Hoistable?**: No for the read itself. Already cheaper than the palette variants since they
  skip `Vdp2SetSpecialPriority`/`Vdp2GetAlpha` entirely (RGB bitmaps have no
  special-priority/special-color-calc mechanism on real hardware).

#### `Vdp2SetSpecialPriority` (vidogl.c:1905)
- **What**: If special-priority mode 2 is active, ORs a priority-bit override into `cramindex`
  by testing the dot value against `PixelIsSpecialPriority`.
- **Called from**: `Vdp2GetPixel4bpp`/`8bpp`/`16bpp` and `Vdp2RotationFetchPixel` — second-level
  per-texel calls.
- **Why per-pixel**: Sub-tile-granular by hardware design — the special-priority bit can differ
  dot-to-dot within the same character/cell.
- **Hoistable?**: No.

#### `Vdp2GetAlpha` (vidogl.c:1919)
- **What**: Per-pixel color-calc alpha; for special-color-calc mode 3, a color-RAM MSB lookup
  via `Vdp2ColorRamGetColorRaw`.
- **Called from**: `Vdp2GetPixel4bpp`/`8bpp`/`16bpp`.
- **Why per-pixel**: Modes 2/3 explicitly vary alpha by the dot's own low nibble or its own
  color-RAM entry's MSB — inherently per-texel.
- **Hoistable?**: No for modes 2/3. Modes 0/1 don't depend on `dot` and could technically be
  hoisted to per-tile/per-row, but the function doesn't special-case that — a minor missed
  optimization, not a correctness issue.
- **Note**: `Vdp2RotationFetchPixel` duplicates this same switch inline instead of calling
  `Vdp2GetAlpha` — code duplication, same per-pixel cost either way.

#### `Vdp2ColorRamGetColorRaw` (vidogl.c:1133)
- **What**: Raw color-RAM word fetch (no RGBA conversion), used to test the MSB
  (translucency) bit.
- **Called from**: `Vdp2GetAlpha`, and directly from `Vdp2RotationFetchPixel`.
- **Why per-pixel**: Special-color-calc mode 3 keys off each pixel's own resolved color-RAM
  entry.
- **Hoistable?**: No.

#### `Vdp2RotationFetchPixel` (vidogl.c:2865)
- **What**: For RBG (rotated background) rendering, fetches one texel at rotated tile-space
  coordinates, resolving palette index, special-priority, and alpha inline.
- **Called from**: `Vdp2DrawMapPerLine` (vidogl.c:3156), `Vdp2DrawMapPerLineNbg23`
  (vidogl.c:3326), `Vdp2DrawRotation_in` (vidogl.c:4147, 4319) — the RBG0/RBG1
  rotation-texture-builder loop.
- **Why per-pixel**: Rotation maps each screen pixel to a non-linear source texel address, so
  the source cell/texel genuinely changes every output pixel — unlike ordinary scrolling NBGs
  this can't be batched per-tile in the general case.
- **Hoistable?**: No in the general case, **but already moved to the GPU where possible**:
  `ygl_texture.cpp`'s `RBGGenerator` compute shader (`prg_rbg_getcolor_4bpp` etc.) implements
  the identical per-texel logic and is used instead whenever `_Ygl->rbg_use_compute_shader` is
  true. `Vdp2DrawRotation_in`'s CPU path is only the fallback for platforms without
  compute-shader support (e.g. macOS/GL4.1).

#### `vdp2rGetKValue` (vidogl.c:3828)
- **What**: Looks up one rotation coefficient (`kx`/`ky`, optionally a line-color address) from
  VRAM/color RAM at the current interpolated K-table index.
- **Called from**: `Vdp2DrawRotation_in`, CPU fallback path only, gated by `coefenab`.
- **Why per-pixel**: The K-table exists specifically to allow sub-tile-granular scale/rotation
  variation (e.g. perspective effects) — must be sampled per output pixel by hardware
  definition.
- **Hoistable?**: No when `coefenab` is set. Already skipped entirely when it's not.

#### `Vdp2PatternAddr` (vidogl.c:2697)
- **What**: Reads a VRAM pattern-name word, decodes character address/palette address/flip
  flags for the current tile.
- **Called from**: `Vdp2DrawRotation_in`, inside the per-pixel loop, but gated by
  `if ((x>>patternshift)!=oldcellx || (y>>patternshift)!=oldcelly)`.
- **Why (conditionally) per-pixel**: Rotation can cross tile boundaries at a non-uniform rate
  (faster than 1 tile per N pixels near a perspective vanishing point), so the tile-change
  *test* must run every pixel even though the VRAM read is amortized.
- **Hoistable?**: The test isn't hoistable; the guard already prevents the expensive read from
  running every pixel — an existing optimization, not a missed one.

#### Confirmed *not* per-pixel CPU work in `vidogl.c`
- **Window testing**: `Vdp2CheckWindowDot` (vidogl.c:1774) would be a true per-pixel window
  test, but its only pixel-loop call site (vidogl.c:4046) is `#if 0`'d out (dead code, with the
  comment "may be faster than GPU"). `Vdp2CheckWindow`/`Vdp2CheckWindowRange` are otherwise
  only called per-tile or per-scanline (building a `vdp2WindowInfo[height]` table and a GPU
  vertex list once per frame). Window masking is done on the GPU (vertex-based window polygons
  / the RBG compute shader's `isWindowInside`, `ygl_texture.cpp:189`).
- **Mosaic**: `ReadMosaicData` runs once per background layer per frame, producing
  `mosaicxmask`/`mosaicymask` uniforms consumed by the fragment shader — no CPU loop snaps
  individual source pixels to a mosaic block.
- **`Vdp2ColorRamGetColor`** (vidogl.c:1153), called from `Vdp2DrawLineColorScreen` inside a
  `for (i=0; i<line_cnt; i++)` loop — that loop iterates **scanlines** (one line-color value
  per row), not pixels. Easy to mistake for a per-pixel call site; it isn't one.
- **VDP1 flat-fill polygon/polyline/line**: `VIDOGLVdp1PolygonDraw`/`PolylineDraw`/`LineDraw`
  build a 1×1-texel `YglTexture` and resolve their fill color exactly once per primitive;
  Gouraud shading is done by uploading 4 interpolated vertex colors for the GPU, not a CPU
  per-pixel shading loop.

---

## `titan/titan.c` — priority-sort/blend compositor

**Used only by `vidsoft.c`.** `vidogl.c` never calls any `Titan*` function (confirmed by grep) —
OpenGL's own blend/depth state composites layers instead. Titan composites the software
renderer's per-layer framebuffers into the final display buffer.

#### `TitanRenderLines` (titan.c:526)
- **What**: For every `(x,y)` in the frame, finds the correct visible layer via `TitanDigPixel`
  and writes the final color.
- **Called from**: `TitanRender`/`TitanRenderThreads`/`TitanRenderSimplifiedCheck`
  (titan.c:206, 618, 663), own `for (y...) for (x...)` loop at titan.c:541-556.
- **Why per-pixel**: Every screen pixel can independently belong to a different priority layer
  (sprite vs. NBG0-3 vs. RBG0 vs. back screen) and needs independent shadow/color-calc
  resolution — genuine Saturn priority-mux behavior.
- **Hoistable?**: Not fundamentally — priority compositing is inherently per-pixel when done in
  software. (`vidogl.c` avoids this class of work entirely by using GPU blend/depth state.)

#### `TitanDigPixel` (titan.c:294)
- **What**: Sorts up to 6 layers by priority for one framebuffer position, resolves
  MSB/normal shadow, blends translucent pixels via `tt_context.blend`.
- **Called from**: `TitanRenderLines`, per pixel (titan.c:550).
- **Why per-pixel**: Priority/shadow/translucency state is stored per-pixel by the upstream
  `TitanPutPixel` calls in `vidsoft.c`.
- **Hoistable?**: No.

#### `TitanBlendPixelsTop` / `TitanBlendPixelsBottom` / `TitanBlendPixelsAdd` (titan.c:230, 248, 268)
- **What**: Alpha-blend or additive-blend two RGBA pixels.
- **Called from**: `TitanDigPixel`, via the `tt_context.blend` function pointer.
- **Why per-pixel**: Blend inputs differ at every pixel by definition of alpha blending.
- **Hoistable?**: No.

#### `TitanTransAlpha` / `TitanTransBit` (titan.c:284, 289)
- **What**: Cheap test of whether a pixel needs blending at all.
- **Called from**: `TitanDigPixel`, via `tt_context.trans`.
- **Why per-pixel**: Skips the more expensive blend call for opaque pixels — already the cheap
  early-out; nothing further to hoist.
- **Hoistable?**: No (already the optimization).

#### `TitanFixAlpha` (titan.c:79/83/93/97, format-dependent variants)
- **What**: Repacks the internal 6-bit-alpha pixel format into the final output format
  (RGB555/565/888).
- **Called from**: `TitanRenderLines` (titan.c:554), `TitanRenderLinesSimplified`
  (titan.c:166, 172, 182, 191).
- **Why per-pixel**: Every output pixel needs this format conversion.
- **Hoistable?**: No — inherent, though cheap (bit-shuffle only).

#### `TitanRenderLinesSimplified` (titan.c:116)
- **What**: Fast compositor path used when color-calc/special-priority/line-screen/shadow are
  all disabled — picks the topmost non-transparent pixel by priority order, skipping
  `TitanDigPixel`'s full sort+blend.
- **Called from**: `TitanRenderSimplifiedCheck` (titan.c:204), own per-pixel loop
  (titan.c:145-198).
- **Why per-pixel**: Same fundamental reason as `TitanRenderLines`, but this path is already
  the hoisted/optimized version — layer sort order is precomputed once per frame outside the
  pixel loop (see comment at titan.c:130).
- **Hoistable?**: Already about as hoisted as software compositing allows; the remaining cost
  (walking every pixel at all) is only avoided by a GPU-side compositor, which is what
  `vidogl.c` uses instead.

---

## Summary: concrete hoisting opportunities found

Not everything here is a hard hardware requirement — these are the specific spots where the
current code pays a per-pixel cost for something that's actually scanline- or frame-constant:

1. **`CheckDil`** (vidsoft.c:2653), called from `putpixel`/`putpixel8` every VDP1 dot, depends
   only on scanline `y` and frame-constant regs — should be hoisted to run once per row.
2. **`vdp1pixelsize` branch inside `DrawLineCallback`** (vidsoft.c:2938, selecting `putpixel`
   vs `putpixel8`) is frame-constant but re-evaluated every dot.
3. **`GenerateRotatedXPosFP`/`GenerateRotatedYPosFP`** (vidshared.h:338/347), called every RBG
   pixel, compute a formula that's affine in screen-x whenever coefficients are disabled or
   per-line-constant (`deltaKAx == 0`) — the per-pixel fixed-point multiply chain could become
   a per-scanline base plus a per-pixel incremental add.
4. **`VidsoftDrawSprite`'s framebuffer-readout-position resolution-combination branch**
   (vidsoft.c:3651-3680) is frame-constant but re-tested every pixel.
5. VDP2 window boundary tests (`TestWindow`/`TestBothWindow` in `vidsoft.c`) use a
   per-scanline-constant clip rectangle but are evaluated as a membership test on every pixel
   across the whole scanline, rather than clipping the loop bounds to `[xstart,xend]` directly.
6. `IsClipped`/`IsUserClipped`/`IsSystemClipped` in VDP1 rasterization test a per-command
   constant rectangle per dot along the Bresenham line; a Cohen-Sutherland-style line-clip
   before rasterizing could avoid touching out-of-rect pixels entirely.
7. `Vdp2GetAlpha` (vidogl.c) doesn't special-case special-color-calc modes 0/1, which don't
   depend on the fetched dot — a minor missed per-tile/per-row hoist.

## Existing hoisting worth recognizing as the right pattern

- `Vdp2MapCalcXY`'s cell memoization (`vidsoft.c:581`) and `Vdp2PatternAddr`'s per-tile guard
  in `Vdp2DrawRotation_in` (`vidogl.c`) — both amortize expensive per-tile decode work across
  many pixels by only re-running it when the source tile actually changes.
- `Vdp2ReadCoefficientFP`'s scanline fast path when `deltaKAx == 0` (`vidsoft.c:1294-1307`).
- The `mosaic_table[16][1024]` lookup table (`vidsoft.c:864-877`).
- `DoColorOffset`/`DoNothing` function-pointer selection hoisted to once-per-line.
- `vidogl.c` moving window testing, mosaic, and (where a compute shader is available) RBG
  sampling off the CPU entirely.
