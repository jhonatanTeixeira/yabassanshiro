# VDP1 Framebuffer System — Complete Study & Overhaul Proposal

## 1. Overview

The VDP1 framebuffer system is the Saturn's sprite/geometry compositing layer. It sits between VDP1 command processing (sprite/polygon/line drawing) and VDP2 background compositing (which reads the VDP1 framebuffer as a texture to composite sprites over backgrounds). The current implementation uses a **double-buffered OpenGL framebuffer** with a CPU-side shadow copy, plus a complex CPU-write-back path for when the Saturn's SH-2 CPUs directly manipulate the framebuffer.

### Key Files

| File | Role |
|------|------|
| `vidogl.c` | VDP1/VDP2 draw orchestration, texture building, NBG/RBG draw functions |
| `ygles.c` | Low-level GL framebuffer management, VDP1 framebuffer read/write, render passes |
| `ygl.h` | `Ygl` struct definition (all GL state, framebuffer objects, uniforms) |
| `ygl_texture.cpp` | Texture atlas management, RBG compute shader |
| `yglshaderes.c` | GLSL shader compilation, uniform binding for framebuffer compositing |
| `vdp1.cpp` | VDP1 command list processing, `Vdp1FrameBuffer` allocation |
| `vdp2.cpp` | VDP2 register emulation, VRAM dirty tracking |

---

## 2. Current Architecture

### 2.1 Double-Buffered Design

```
VDP1 Framebuffer (Saturn hardware): 512KB VRAM, 16-bit color
    |
    v
Two CPU-side shadow buffers: Vdp1FrameBuffer[0] and Vdp1FrameBuffer[1]
    (u8*, 0x40000 bytes each = 256KB, allocated in vdp1.cpp)
    |
    v
Two OpenGL textures: _Ygl->vdp1FrameBuff[0] and _Ygl->vdp1FrameBuff[1]
    (GLuint, RGBA8, resolution-dependent size)
    |
    v
One shared FBO: _Ygl->vdp1fbo (attaches one of the two textures at a time)
    + depth renderbuffer: _Ygl->rboid_depth
    + stencil renderbuffer: _Ygl->rboid_stencil
```

**Frame roles** (tracked by `_Ygl->drawframe` and `_Ygl->readframe`):

| Variable | Meaning |
|----------|---------|
| `_Ygl->drawframe` | Index of the buffer being **drawn to** (VDP1 commands render here) |
| `_Ygl->readframe` | Index of the buffer being **read from** (VDP2 composites from here) |

**Swap** happens in `YglFrameChangeVDP1()` (ygles.c:2995):
```c
void YglFrameChangeVDP1() {
    u32 current_drawframe = _Ygl->drawframe;
    _Ygl->drawframe = _Ygl->readframe;
    _Ygl->readframe = current_drawframe;
}
```

### 2.2 Per-Frame Lifecycle

```
1. VIDOGLVdp1DrawStart()        -- freeze VDP2 regs, process VDP1 command list
2.   Vdp1DrawCommands()          -- calls draw callbacks for each VDP1 command
3.     VIDOGLVdp1NormalSpriteDraw() etc. -- build textures, queue GL quads
4. VIDOGLVdp1DrawEnd()           -- push textures, call YglRenderVDP1()
5.   YglRenderVDP1()             -- render all queued VDP1 quads to vdp1FrameBuff[drawframe]
6. YglFrameChangeVDP1()          -- swap drawframe <-> readframe
7. VIDOGLVdp2DrawStart()         -- freeze VDP2 regs, manage texture atlas cache
8. VIDOGLVdp2DrawScreens()       -- draw all VDP2 layers
9.   Vdp2DrawBackScreen()        -- clear/background color
10.  Vdp2DrawNBG0..3()           -- tile/bitmap backgrounds
11.  Vdp2DrawRBG0()              -- rotated backgrounds
12. YglRender()                  -- composite VDP1 framebuffer over VDP2 layers
13.   YglRenderFrameBuffer()     -- draw vdp1FrameBuff[readframe] as textured quad
14.   YglRenderFrameBufferShadow() -- MSB shadow pass
15. VIDOGLVdp2DrawEnd()          -- finalize
```

### 2.3 CPU Write-Back Path

The Saturn's SH-2 CPUs can directly write to VDP1 framebuffer memory (used for effects, software rendering, etc.). This is handled by `VIDOGLVdp1WriteFrameBuffer()` (ygles.c:844):

1. Writes to `Vdp1FrameBuffer[drawframe]` (CPU-side shadow, raw Saturn format)
2. Converts Saturn 16-bit color to RGBA8 in `_Ygl->CpuWriteFrameBuffer[]`
3. Sets `_Ygl->cpu_framebuffer_write[drawframe]++` (dirty counter)

Before any render pass that reads a framebuffer, `YglDrawCpuFramebufferWrite()` (ygles.c:971) checks this counter and, if non-zero:
1. Uploads `CpuWriteFrameBuffer` as a texture (`smallfbotex`)
2. Blits it onto the target FBO via `YglWindowFramebuffer()`
3. Clears the dirty counter

### 2.4 CPU Read-Back Path

`VIDOGLVdp1ReadFrameBuffer()` (ygles.c:1016) handles Saturn CPU reads from the framebuffer:

1. If the draw buffer has CPU writes pending, reads from `Vdp1FrameBuffer[drawframe]` (CPU shadow)
2. Otherwise, reads back from the GPU via `glReadPixels` through a PBO (`vdp1pixelBufferID`) and a small intermediate FBO (`smallfbo`)

The read path is complex because the GPU texture may have been modified by VDP1 rendering (which the CPU shadow doesn't reflect), so it must round-trip through the GPU.

### 2.5 MSB Shadow System

The Saturn VDP1 has an MSB (Most Significant Bit) shadow feature: when a sprite pixel's MSB is set, it marks that pixel as a "shadow" that affects how VDP2 composites it. The emulator tracks this via:

- `_Ygl->msb_shadow_count_[2]` — per-buffer count of MSB-shadow sprites
- `YglRenderFrameBufferShadow()` (ygles.c:3605) — a second render pass that draws the framebuffer with a shadow-specific shader
- The shadow pass is gated on `msb_shadow_count_[readframe] > 0`

### 2.6 Framebuffer Uniforms

The `UniformFrameBuffer` struct (ygl.h:439) packs per-frame parameters into a UBO:
- Per-priority alpha values (8 entries, from CCRSA)
- Per-priority depth values (8 entries, from PRISA)
- Color offset (from COAR/COBR etc.)
- Sprite window enable flag
- Viewport/height info
- Color RAM offset

These are uploaded in `YglUpdateVdp2Reg()` (ygles.c:3155) and consumed by the framebuffer compositing shaders.

---

## 3. Performance Problems

### 3.1 CPU Read-Back is Expensive

`VIDOGLVdp1ReadFrameBuffer()` uses `glReadPixels` through a PBO, which:
- Forces a GPU-to-CPU sync (stalls the pipeline)
- Requires an intermediate FBO (`smallfbo`) and texture (`smallfbotex`)
- Copies the entire framebuffer even when only one pixel is needed
- The PBO path (`glMapBuffer`) adds additional overhead

**Impact**: Games that frequently read the VDP1 framebuffer (e.g., for collision detection, software effects) pay a heavy GPU sync penalty every frame.

### 3.2 CPU Write-Back is Redundant

`VIDOGLVdp1WriteFrameBuffer()`:
1. Writes to `Vdp1FrameBuffer[drawframe]` (raw Saturn format)
2. Converts to RGBA8 in `CpuWriteFrameBuffer`
3. Uploads as a texture
4. Blits onto the FBO

Steps 2-4 are wasted if the same pixels are then overwritten by VDP1 rendering (which happens in most cases — the CPU writes to the framebuffer, then VDP1 draws sprites over it).

### 3.3 Double Shadow Copy

The framebuffer exists in **three** representations simultaneously:
1. `Vdp1FrameBuffer[]` — raw Saturn 16-bit format (CPU shadow)
2. `vdp1FrameBuff[]` — OpenGL texture (GPU copy)
3. `CpuWriteFrameBuffer` — RGBA8 CPU buffer (for CPU write-back)

The CPU shadow (`Vdp1FrameBuffer[]`) is only needed for CPU reads/writes. For games that never touch the framebuffer from the CPU, it's pure waste.

### 3.4 Full-Framebuffer Operations

- `YglEraseWriteVDP1()` clears the entire framebuffer with `glClear()` every frame
- `YglRenderFrameBuffer()` draws the entire framebuffer as a full-screen quad
- `YglRenderFrameBufferShadow()` does a second full-screen pass when MSB shadows are active
- The PBO read-back in `VIDOGLVdp1ReadFrameBuffer()` can read the entire buffer

None of these operations are scoped to dirty regions.

### 3.5 FBO Reconfiguration Overhead

The shared `vdp1fbo` is reconfigured multiple times per frame:
- In `YglEraseWriteVDP1()` — attaches `vdp1FrameBuff[readframe]`
- In `YglRenderVDP1()` — attaches `vdp1FrameBuff[drawframe]`
- In `YglDrawCpuFramebufferWrite()` — attaches target buffer
- In `VIDOGLVdp1ReadFrameBuffer()` — may attach `smallfbo`

Each `glFramebufferTexture2D` call can trigger driver-internal validation.

### 3.6 Resolution Mismatch

The VDP1 framebuffer is always rendered at the Saturn's native resolution (e.g., 320×224), but the VDP2 compositing may be at a higher resolution (e.g., 640×448 in hi-res interlace mode). The `vdp1wratio`/`vdp1hratio` scaling factors handle this, but the intermediate `smallfbo` blit adds an extra pass.

---

## 4. Overhaul Proposals

### 4.1 Proposal A: Lazy CPU Shadow + Dirty-Rect Tracking

**Goal**: Eliminate unnecessary CPU↔GPU transfers.

**Changes**:

1. **Deferred CPU shadow allocation**: Only allocate `Vdp1FrameBuffer[2]` when a CPU read/write to the framebuffer is first detected. For games that never touch it, save 512KB of RAM and avoid all sync overhead.

2. **Dirty-rect tracking for CPU writes**: Instead of a simple counter (`cpu_framebuffer_write[]`), track a dirty rectangle (min_x, min_y, max_x, max_y). When uploading CPU writes to the GPU, only `glTexSubImage2D` the dirty region instead of the full buffer.

3. **Dirty-rect tracking for GPU reads**: When the CPU reads the framebuffer and the GPU has rendered to it since the last read, only `glReadPixels` the region actually needed (the single pixel or small rect the CPU requested), not the full buffer.

4. **Merge CPU writes into the VDP1 command stream**: Instead of a separate blit pass, encode CPU framebuffer writes as VDP1-style textured quads and append them to the VDP1 draw list. This eliminates the FBO switch and extra draw call.

**Complexity**: Medium
**Impact**: Medium (helps CPU-framebuffer-heavy games, no change for others)
**Risk**: Low (behavior-preserving, just scopes operations tighter)

### 4.2 Proposal B: Single-Buffer Mode with No CPU Shadow

**Goal**: Eliminate the double-buffer swap and CPU shadow entirely for the common case.

**Changes**:

1. **Detect at runtime whether double-buffering is needed**: The Saturn VDP1 has a `PTMR` register that controls framebuffer switching. Many games use a simple single-buffer mode where the same buffer is drawn to and read from. Detect this and use a single FBO+texture.

2. **Eliminate `Vdp1FrameBuffer[]` allocation when not needed**: If no CPU framebuffer access occurs during the first few frames, never allocate the CPU shadow. Use a `mprotect`-based trap (SIGSEGV handler) to lazily detect the first CPU access and allocate+sync at that point.

3. **Inline the framebuffer swap into the render pass**: Instead of `YglFrameChangeVDP1()` as a separate step, make the swap implicit in `YglRenderVDP1()` — render to the "other" buffer, then atomically swap the read/draw roles.

**Complexity**: High
**Impact**: High (saves memory bandwidth, eliminates FBO reconfig)
**Risk**: Medium (needs careful handling of edge cases like MSB shadows and CPU access)

### 4.3 Proposal C: GPU-Only Framebuffer with Compute Shader Read-Back

**Goal**: Eliminate all CPU involvement in framebuffer management.

**Changes**:

1. **Remove `Vdp1FrameBuffer[]` entirely**: The CPU shadow is only needed for CPU reads/writes. Handle CPU reads via a compute shader that writes the requested pixel to a small SSBO, then map that SSBO. This avoids the full-buffer `glReadPixels`.

2. **Handle CPU writes via a compute shader**: When the CPU writes to the framebuffer, encode the write as a small compute shader dispatch that updates the GPU texture directly. No CPU-side RGBA8 conversion, no texture upload, no blit.

3. **Use `GL_ARB_texture_storage` + `glTextureSubImage2D` with persistent mapping**: If the driver supports it, use a persistently mapped texture for the framebuffer, avoiding explicit upload/download calls.

4. **Eliminate the intermediate `smallfbo`**: The CPU write-back blit (`YglWindowFramebuffer`) exists because the CPU writes to a separate buffer. With compute-shader-based writes, writes go directly to the framebuffer texture, so no blit is needed.

**Complexity**: Very High
**Impact**: Very High (eliminates all CPU-GPU sync for framebuffer access)
**Risk**: High (compute shader availability varies by platform/GL version; fallback needed)

### 4.4 Proposal D: Tiled / Dirty-Region Rendering

**Goal**: Only render and composite the parts of the framebuffer that actually changed.

**Changes**:

1. **Track VDP1 command bounding boxes**: As VDP1 commands are processed, accumulate a dirty region (bounding box of all sprites/polygons drawn this frame). Only clear and render within this region.

2. **Scissored clears**: Replace `glClear(GL_COLOR_BUFFER_BIT)` with `glScissor` + `glClear` limited to the dirty region.

3. **Scissored framebuffer compositing**: In `YglRenderFrameBuffer()`, only draw the portion of the framebuffer that actually changed. This requires modifying the vertex coordinates and texture coordinates to match the dirty region.

4. **Tile-based invalidation for the texture cache**: Instead of invalidating the entire atlas, track which 64×64 or 128×128 tiles of the framebuffer are dirty and only rebuild those.

**Complexity**: Medium-High
**Impact**: Medium (helps static scenes significantly, less impact on full-screen action)
**Risk**: Low-Medium (scissor-based, easy to fall back to full-frame)

### 4.5 Proposal E: Unified Framebuffer Format

**Goal**: Eliminate the Saturn-to-RGBA8 conversion overhead.

**Changes**:

1. **Use a 16-bit GPU texture format**: Instead of `GL_RGBA8`, use `GL_RGB565` or `GL_RGBA4` for the framebuffer texture. This matches the Saturn's native 16-bit color and eliminates the conversion in `Vdp1ReadTexture()` and `VIDOGLVdp1WriteFrameBuffer()`.

2. **Move color conversion to the shader**: The fragment shader that composites the VDP1 framebuffer over VDP2 backgrounds would need to handle the 16-bit→RGBA8 conversion. This is already partially done (the shader reads `s_vdp1FrameBuffer` as a texture), but the texel data is pre-converted.

3. **Eliminate `VDP1COLOR()` macro calls**: The `VDP1COLOR()` macro (which packs priority, color-calc mode, shadow bits, and color into a 32-bit RGBA8 value) is called per-texel in `Vdp1ReadTexture()`. Moving this to the shader would save significant CPU time.

**Complexity**: High
**Impact**: High (eliminates per-texel CPU color conversion)
**Risk**: High (changes the entire texture pipeline; shaders must exactly reproduce the VDP1COLOR encoding)

### 4.6 Proposal F: Persistent Mapped Buffer for CPU Framebuffer Access

**Goal**: Eliminate PBO map/unmap and glReadPixels overhead.

**Changes**:

1. **Use `GL_MAP_PERSISTENT_BIT` + `GL_MAP_COHERENT_BIT`**: Create the framebuffer textures with a persistently mapped buffer backing. This allows the CPU to read/write pixels directly without explicit sync.

2. **Replace `glReadPixels` with direct buffer reads**: When the CPU needs to read a pixel, read it directly from the mapped buffer. No FBO switch, no PBO, no sync.

3. **Replace `glTexSubImage2D` with direct buffer writes**: When the CPU writes a pixel, write it directly to the mapped buffer. The GPU sees the update automatically (coherent mapping).

**Requirements**: `GL_ARB_buffer_storage` or `GL_EXT_buffer_storage` (OpenGL 4.4+ / GLES 3.1+)
**Complexity**: Medium
**Impact**: High (eliminates all explicit CPU-GPU transfers for framebuffer access)
**Risk**: Medium (driver support varies; fallback needed for older GL versions)

---

## 5. Recommended Implementation Order

### Phase 1: Quick Wins (Low Risk, Immediate Payoff)

1. **Dirty-rect tracking for CPU writes** (part of Proposal A)
   - Replace `cpu_framebuffer_write[]` counter with a dirty rectangle
   - Only upload the dirty region in `YglDrawCpuFramebufferWrite()`
   - **Files**: `ygles.c`, `ygl.h`
   - **Estimated gain**: Small but consistent for CPU-framebuffer-heavy games

2. **Scissored framebuffer clears** (part of Proposal D)
   - Track VDP1 command bounding boxes in `VIDOGLVdp1DrawStart()`
   - Use `glScissor` + `glClear` instead of full `glClear`
   - **Files**: `vidogl.c`, `ygles.c`
   - **Estimated gain**: Small for static scenes, negligible for full-screen action

3. **Lazy CPU shadow allocation** (part of Proposal A)
   - Only allocate `Vdp1FrameBuffer[2]` on first CPU access
   - **Files**: `vdp1.cpp`, `ygles.c`
   - **Estimated gain**: Saves 512KB RAM, no perf impact for most games

### Phase 2: Structural Changes (Medium Risk, Significant Payoff)

4. **Persistent mapped buffer for framebuffer** (Proposal F)
   - Replace PBO-based read-back with persistent mapping
   - Replace `glTexSubImage2D` CPU write-back with direct buffer writes
   - **Files**: `ygles.c`, `ygl.h`
   - **Estimated gain**: Eliminates GPU sync stalls on framebuffer access

5. **Inline framebuffer swap** (part of Proposal B)
   - Make `YglFrameChangeVDP1()` implicit in `YglRenderVDP1()`
   - **Files**: `ygles.c`, `vidogl.c`
   - **Estimated gain**: Eliminates redundant FBO reconfiguration

6. **Merge CPU writes into VDP1 command stream** (part of Proposal A)
   - Instead of a separate blit, encode CPU framebuffer writes as textured quads
   - **Files**: `ygles.c`, `vidogl.c`
   - **Estimated gain**: Eliminates extra draw call and FBO switch

### Phase 3: Advanced Optimizations (High Risk, Highest Payoff)

7. **Unified 16-bit framebuffer format** (Proposal E)
   - Use `GL_RGB565` framebuffer texture
   - Move color conversion to shader
   - **Files**: `ygles.c`, `yglshaderes.c`, `vidogl.c`, `ygl.h`
   - **Estimated gain**: Eliminates per-texel CPU color conversion in `Vdp1ReadTexture()`

8. **Compute shader framebuffer access** (Proposal C)
   - Replace CPU read/write paths with compute shader dispatches
   - **Files**: `ygles.c`, `yglshaderes.c`
   - **Estimated gain**: Eliminates all CPU-GPU sync for framebuffer access

9. **Tiled dirty-region rendering** (full Proposal D)
   - Full scissored/tiled rendering for both VDP1 and VDP2
   - **Files**: `vidogl.c`, `ygles.c`, `vdp1.cpp`, `vdp2.cpp`
   - **Estimated gain**: Significant for static/partially-updated scenes

---

## 6. Key Data Structures

### Current (ygl.h:577-665)

```c
// VDP1 Framebuffer
int rwidth;                          // Render width (e.g., 320)
int rheight;                         // Render height (e.g., 224)
int density;                         // Resolution multiplier
int drawframe;                       // Current draw buffer index (0 or 1)
int readframe;                       // Current read buffer index (0 or 1)
GLuint rboid_depth;                  // Depth renderbuffer
GLuint rboid_stencil;                // Stencil renderbuffer
GLuint vdp1fbo;                      // Shared VDP1 FBO
GLuint vdp1FrameBuff[2];             // Two color attachment textures
GLuint smallfbo;                     // Intermediate FBO for CPU write-back
GLuint smallfbotex;                  // Intermediate texture for CPU write-back
GLuint vdp1pixelBufferID;            // PBO for GPU read-back
void * pFrameBuffer;                 // Mapped PBO pointer (or NULL)

// CPU framebuffer write path
int cpu_framebuffer_write[2];        // Per-buffer dirty counter
int bWriteCpuFrameBuffer;            // Flag: CPU wrote to framebuffer this frame
u32 * CpuWriteFrameBuffer;           // RGBA8 CPU-side copy for write-back

// MSB shadow
int msb_shadow_count_[2];            // Per-buffer MSB shadow sprite count

// Uniforms
UniformFrameBuffer fbu_;             // Per-frame UBO data
GLuint framebuffer_uniform_id_;      // UBO handle
```

### Proposed Additions

```c
// Dirty region tracking
struct {
    int x1, y1, x2, y2;             // Dirty rectangle (inclusive)
    int valid;                       // Whether the rect is initialized
} fb_dirty_rect[2];                  // Per-buffer dirty rect

// Lazy allocation
int cpu_shadow_allocated;            // Whether Vdp1FrameBuffer[] is allocated
int cpu_access_detected;             // Whether CPU has accessed the framebuffer

// Persistent mapping (Proposal F)
GLuint fb_buffer_storage;            // Buffer object for persistent mapping
void * fb_mapped_ptr;                // Persistently mapped pointer
int persistent_mapping_supported;    // Runtime check

// Tiled rendering (Proposal D)
int fb_tile_size;                    // Tile size (e.g., 64)
int fb_dirty_tiles[(1024/64)*(512/64)]; // Bitmask of dirty tiles
```

---

## 7. Shader Impact

The framebuffer compositing shaders (in `yglshaderes.c`) currently expect RGBA8 textures with pre-encoded priority/alpha/color-calc information in the pixel format. Any change to the framebuffer format (Proposal E) or the encoding scheme would require updating:

- `PG_VDP2_DRAWFRAMEBUFF` — main framebuffer compositing shader
- `PG_VDP2_DRAWFRAMEBUFF_ADDCOLOR` — add-color mode shader
- `PG_VDP2_DRAWFRAMEBUFF_ADDCOLOR_SHADOW` — shadow pass shader
- `PG_VDP2_DRAWFRAMEBUFF_DESTINATION_ALPHA` — destination alpha shader
- `PG_VDP2_DRAWFRAMEBUFF_LINECOLOR` — line-color screen shader
- `PG_VDP2_DRAWFRAMEBUFF_SHADOW` — MSB shadow shader

The `UniformFrameBuffer` UBO would also need updating if the encoding changes.

---

## 8. Testing Strategy

Each phase should be validated against the existing baseline:

1. **Visual correctness**: Run *Magic Knight Rayearth (USA)* and compare output pixel-for-pixel with the unmodified build
2. **Performance**: Measure `ps -o %cpu` on the RetroArch process
3. **Regression test**: Run a set of known-problematic titles:
   - *Virtua Fighter 2* (heavy VDP1 usage, MSB shadows)
   - *Sega Rally* (rotation backgrounds, coefficient tables)
   - *NiGHTS into Dreams* (line-scroll, bitmap modes)
   - *Panzer Dragoon* (transparency effects)
   - *Shining Force III* (CPU framebuffer access for battle maps)
4. **CPU framebuffer access test**: A title known to read/write the framebuffer from the SH-2 (e.g., *Burning Rangers* for its software-transparency effects)

---

## 9. Risk Assessment

| Proposal | Performance Gain | Code Complexity | Regression Risk | Platform Portability |
|----------|-----------------|-----------------|-----------------|---------------------|
| A (Lazy + Dirty-Rect) | Medium | Low | Low | High |
| B (Single-Buffer) | High | Medium | Medium | High |
| C (Compute Shader) | Very High | Very High | High | Low (GLES 3.1+) |
| D (Tiled Rendering) | Medium | Medium | Low-Medium | High |
| E (16-bit Format) | High | High | High | Medium |
| F (Persistent Mapping) | High | Medium | Medium | Medium (GL 4.4+) |

**Recommended first step**: Phase 1 (dirty-rect tracking + scissored clears + lazy allocation) — these are safe, well-understood changes that provide immediate benefit and lay the groundwork for the more aggressive optimizations in Phases 2 and 3.

---

## 10. Appendix: Current Code Flow Diagrams

### 10.1 VDP1 Framebuffer Write (CPU → GPU)

```
SH-2 CPU writes to VDP1 framebuffer address
    |
    v
MappedMemoryWriteWord() in memory.c
    |
    v
Vdp1RamWriteWord() in vdp1.cpp
    |
    v
VIDOGLVdp1WriteFrameBuffer(type, addr, val) in ygles.c:844
    |
    +---> T1WriteByte/Word/Long(Vdp1FrameBuffer[drawframe], addr, val)
    |         (CPU shadow, raw Saturn format)
    |
    +---> Convert Saturn 16-bit -> RGBA8 in CpuWriteFrameBuffer[]
    |         (per-pixel VDP1COLOR encoding)
    |
    +---> cpu_framebuffer_write[drawframe]++
    |
    v
[Later, in YglRenderVDP1() or YglRenderFrameBuffer():]
    |
YglDrawCpuFramebufferWrite(target) in ygles.c:971
    |
    +---> glTexSubImage2D(CpuWriteFrameBuffer -> smallfbotex)
    +---> YglWindowFramebuffer(smallfbotex -> vdp1fbo)
    +---> Clear cpu_framebuffer_write[]
```

### 10.2 VDP1 Framebuffer Read (GPU → CPU)

```
SH-2 CPU reads from VDP1 framebuffer address
    |
    v
MappedMemoryReadWord() in memory.c
    |
    v
Vdp1RamReadWord() in vdp1.cpp
    |
    v
VIDOGLVdp1ReadFrameBuffer(type, addr, out) in ygles.c:1016
    |
    +---> If cpu_framebuffer_write[drawframe] > 0:
    |         Read from Vdp1FrameBuffer[drawframe] directly (CPU shadow is current)
    |
    +---> Else (GPU has rendered since last CPU write):
              |
              +---> glBindFramebuffer(vdp1fbo)
              +---> glFramebufferTexture2D(vdp1FrameBuff[drawframe])
              +---> glBlitFramebuffer(vdp1fbo -> smallfbo)
              +---> glBindBuffer(PIXEL_PACK_BUFFER, vdp1pixelBufferID)
              +---> glReadPixels(smallfbo -> PBO)
              +---> glMapBuffer(PBO) -> pFrameBuffer
              +---> Read pixel from pFrameBuffer
              +---> glUnmapBuffer(PBO)
```

### 10.3 VDP1 Render Pass

```
VIDOGLVdp1DrawEnd() in vidogl.c:4610
    |
    +---> YglTmPush(YglTM)          -- upload texture atlas
    +---> YglRenderVDP1()           -- render all queued VDP1 geometry
              |
              +---> YglGenFrameBuffer()         -- ensure FBO exists
              +---> YglDrawCpuFramebufferWrite() -- flush CPU writes
              +---> glBindFramebuffer(vdp1fbo)
              +---> glFramebufferTexture2D(vdp1FrameBuff[drawframe])
              +---> For each draw level:
              +--->   glDrawArrays(GL_TRIANGLES) -- render sprites
              +---> glFenceSync()
              +---> glBindFramebuffer(default_fbo)
    |
    +---> YglFrameChangeVDP1()      -- swap drawframe <-> readframe
```

### 10.4 VDP2 Compositing Pass (reads VDP1 framebuffer)

```
YglRender() in ygles.c:3676
    |
    +---> For each priority level (0..7):
    |         |
    |         +---> YglRenderFrameBuffer(from, to) in ygles.c:3355
    |         |         |
    |         |         +---> YglGenFrameBuffer()
    |         |         +---> YglDrawCpuFramebufferWrite(readframe)
    |         |         +---> Bind vdp1FrameBuff[readframe] as texture
    |         |         +---> Set up stencil for window testing
    |         |         +---> Draw full-screen textured quad
    |         |         +---> (Optional) Second pass for color-calc window
    |         |
    |         +---> (VDP2 background layers interleaved by priority)
    |
    +---> If msb_shadow_count_[readframe] > 0:
    |         YglRenderFrameBufferShadow() -- shadow pass
    |
    +---> (Optional) FXAA post-process
    +---> YglBlitFramebuffer() to default_fbo
```
