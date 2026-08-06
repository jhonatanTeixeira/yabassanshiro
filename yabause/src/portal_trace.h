/* Portal to Another World — structured, real-time recompilation trace.
 *
 * Emits one JSON object per line (JSONL) describing exactly the events the
 * Rust recompiler (../../../src/) needs to build itself against: which
 * function got called with what register/argument state, and which
 * hardware-relevant memory region got written with what value — instead of
 * the bare "first-seen PC" list that traces/frames/*.txt already provides.
 *
 * Intentionally NOT logging every VRAM/VDP1 command-table byte write (far
 * too voluminous, and src/memory.rs already has its own VDP1 command-table
 * parser) — only register-level VDP1/VDP2/SCU/SMPC *control register*
 * writes, plus every call/return, which is what turns into HLE code.
 *
 * See docs/roadmap_press_start.md and tools/study_frame.py for how the
 * output of this feeds the existing frame-study pipeline.
 *
 * Enabled only when built with `PORTAL_TRACE=1` (see libretro/Makefile) —
 * zero-cost / compiles to nothing in a normal build.
 */
#ifndef PORTAL_TRACE_H
#define PORTAL_TRACE_H

#include "core.h"

#if defined(PORTAL_TRACE)

#ifdef __cplusplus
extern "C" {
#endif

void PortalTraceInit(void);
void PortalTraceFrame(u64 frame);
/* Logs a confirmed JSR/BSR/JMP/BRAF/BSRF. The opcode-family test itself
 * must stay inline at the call site (sh2int.c's per-instruction dispatch
 * loop) - see the big comment above this function's definition in
 * portal_trace.c for why calling into this translation unit on *every*
 * instruction (rather than only on confirmed calls) broke real-time timing
 * badly enough to keep the game on a black screen. */
void PortalTraceLogCall(u32 pc, u32 target, const char *mnem);
void PortalTraceReturn(void);
void PortalTraceMemWrite(u32 addr, u32 val, int width, const char *region);
void PortalTraceClose(void);
/* Toggled by the F9 keyboard hotkey (libretro.c's keyboard callback) - stop
 * finalizes the current recording segment, start begins a genuinely new
 * one. See the comment on its definition in portal_trace.c. */
void PortalTraceToggleCapture(void);

#ifdef __cplusplus
}
#endif

#else

#define PortalTraceInit()
#define PortalTraceFrame(frame) ((void)(frame))
#define PortalTraceLogCall(pc, target, mnem) ((void)(pc), (void)(target), (void)(mnem))
#define PortalTraceReturn()
#define PortalTraceMemWrite(addr, val, width, region) ((void)(addr), (void)(val), (void)(width), (void)(region))
#define PortalTraceClose()
#define PortalTraceToggleCapture()

#endif

#endif
