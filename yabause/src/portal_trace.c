/* See portal_trace.h for the "why". */
#include "portal_trace.h"

#if defined(PORTAL_TRACE)

#include <stdio.h>
#include <stdlib.h>
#include "sh2core.h"

static FILE *g_fp = NULL;
static u64 g_frame = 0;

/* Live capture on/off, toggled by a keyboard hotkey (F9, see libretro.c's
 * portal_trace_keyboard_event) rather than only compile-time/env-var
 * control - press once to stop the CURRENT recording (closes it cleanly,
 * so it's immediately mergeable/inspectable), press again to start a NEW
 * one (fresh file, never reopens/appends to a stopped one - each on/off
 * cycle becomes its own distinct .jsonl, which is exactly what
 * tools/ingest_portal_trace.py's directory-merge mode wants to consume:
 * `python3 tools/ingest_portal_trace.py traces/portal_sessions/`). Starts
 * ON automatically at boot ("capture from wherever I go" without having to
 * remember to press anything first). */
static int g_capturing = 1;
static int g_segment = 0;
static char g_base_path[512] = {0};

/* Skip everything happening inside the BIOS ROM (both the cache-enabled
 * 0x0xxxxxxx window and its cache-through 0x2xxxxxxx mirror — see
 * docs/address_mapping.md). The project has already decided BIOS fidelity
 * is out of scope ("não sei se agente precisa de código de bios"); leaving
 * this off by default preserves raw ground truth, but interactive sessions
 * should normally set PORTAL_TRACE_SKIP_BIOS=1 to cut ~90% of the noise
 * a cold boot produces before the game's own code is even reached. */
static int g_skip_bios = -1; /* -1 = not read yet, 0 = off, 1 = on */

static int SkipBios(void) {
  if (g_skip_bios < 0) {
    const char *v = getenv("PORTAL_TRACE_SKIP_BIOS");
    g_skip_bios = (v && v[0] == '1') ? 1 : 0;
  }
  return g_skip_bios;
}

static int InBios(u32 addr) {
  return (addr & 0x0FFFFFFF) < 0x00100000;
}

/* fflush() after every single event is fine for the short automated smoke
 * tests this was originally written for, but during real interactive play
 * (this is now driven live, at 60fps, from a controller) it turns every
 * JSR/mem write into a blocking syscall and stalls frame pacing badly.
 * Buffer normally; flush periodically (bounds data loss on a hard crash to
 * at most this many lines) and always on PortalTraceClose(). */
#define PORTAL_TRACE_FLUSH_EVERY 4096
static u32 g_pending = 0;

static void MaybeFlush(void) {
  if (++g_pending >= PORTAL_TRACE_FLUSH_EVERY) {
    fflush(g_fp);
    g_pending = 0;
  }
}

static void OpenSegment(void) {
  char path[560];
  snprintf(path, sizeof(path), "%s_%04d.jsonl", g_base_path, g_segment);
  g_fp = fopen(path, "w");
  if (g_fp) {
    /* Fully-buffered even though stdout may be a terminal; this is always a
     * regular file. Larger buffer = fewer write() syscalls during play. */
    setvbuf(g_fp, NULL, _IOFBF, 1 << 20);
    fprintf(g_fp, "{\"type\":\"session_start\"}\n");
    g_pending = 0;
    fprintf(stderr, "[portal_trace] recording segment %d -> %s\n", g_segment, path);
  } else {
    fprintf(stderr, "[portal_trace] failed to open %s for writing\n", path);
  }
}

void PortalTraceInit(void) {
  const char *path = getenv("PORTAL_TRACE_PATH");
  /* Default lands inside the repo's own traces/portal_sessions/ (already
   * fully gitignored - see .gitignore's /traces entry), NOT /tmp: a single
   * real play session produces multi-GB files, and /tmp has repeatedly
   * been the smaller/nearly-full disk in practice (this project's root
   * partition sat at 2.8GB free after one session). Relative path assumes
   * retroarch is launched from the repo root, which is the documented
   * convention (see .agents/skills/portal-trace-capture/SKILL.md) - set
   * PORTAL_TRACE_PATH explicitly if launching from elsewhere.
   *
   * g_base_path is a PREFIX, not a full filename - each recording segment
   * (one on/off cycle of the F9 toggle) becomes "<prefix>_NNNN.jsonl". If
   * PORTAL_TRACE_PATH is set to something ending in ".jsonl" (the old
   * single-file convention), strip that suffix so both still work the same
   * way under the hood. */
  if (!path) path = "traces/portal_sessions/session";
  size_t len = strlen(path);
  if (len > 6 && strcmp(path + len - 6, ".jsonl") == 0) len -= 6;
  if (len >= sizeof(g_base_path)) len = sizeof(g_base_path) - 1;
  memcpy(g_base_path, path, len);
  g_base_path[len] = '\0';

  g_capturing = 1;
  g_segment = 0;
  OpenSegment();
}

/* Called from libretro.c's keyboard callback on an F9 keydown (rising edge
 * only - see there). Toggle, not a plain rotate: stop closes the current
 * segment cleanly (immediately mergeable/inspectable, nothing left
 * buffered-but-unflushed); start begins a genuinely NEW file rather than
 * reopening the one that was just stopped, so every on/off cycle is its
 * own self-contained recording. */
void PortalTraceToggleCapture(void) {
  if (g_capturing) {
    if (g_fp) { fflush(g_fp); fclose(g_fp); g_fp = NULL; }
    g_capturing = 0;
    fprintf(stderr, "[portal_trace] capture STOPPED (segment %d finalized)\n", g_segment);
  } else {
    g_segment++;
    g_capturing = 1;
    OpenSegment();
  }
}

void PortalTraceFrame(u64 frame) {
  g_frame = frame;
  if (!g_fp) return;
  fprintf(g_fp, "{\"type\":\"frame\",\"n\":%llu}\n", (unsigned long long)frame);
  MaybeFlush();
}

/* Root-caused 2026-08-05: this used to be PortalTraceCheckCall(u16 opcode),
 * called unconditionally from sh2int.c's per-instruction dispatch loop for
 * *every* instruction the master SH-2 fetches, with the JSR/JMP/BRAF/BSRF/
 * BSR pattern match done in here. That is a real cross-translation-unit
 * function call on the single hottest path in the whole emulator - unlike
 * the pre-existing, proven-safe TRACE_INTERP_PC hook's TraceMarkSeen(),
 * which is `static` inside sh2int.c itself and gets fully inlined away.
 * Even with this function's body reduced to a bare `return;` (verified via
 * bisection - see git history), the call-boundary cost alone was enough to
 * perturb some real-time-sensitive subsystem (almost certainly CD-block
 * read-latency emulation, cs2.c) and desync it from what the game expects:
 * a real play/headless-screenshot session that should show the opening
 * cinematic by frame ~2123 stayed on a black loading screen instead.
 *
 * Fix: sh2int.c now does the cheap opcode-family test itself, inline, and
 * only calls into this translation unit (PortalTraceLogCall) on an actual
 * confirmed call instruction - not on every instruction fetch. */
/* Overlay-region call target detection - see the big comment on
 * FormatMemDump() below for the "why". 0x060D0000 is where
 * extraction/0.BIN's own real content ends (confirmed 2026-08-06: 835584
 * bytes from 0x06004000, matching the source ISO's own recorded size for
 * "/0;1" exactly - NOT a truncated extraction), so any call landing at or
 * past it is executing out of whatever OL*.BIN overlay is currently
 * resident there, never out of 0.BIN itself. */
#define OVERLAY_REGION_START 0x060D0000
#define MEM_DUMP_BYTES 64

static int IsOverlayRegion(u32 addr) {
  return (addr & 0x0FFFFFFF) >= (OVERLAY_REGION_START & 0x0FFFFFFF);
}

/* Dumps MEM_DUMP_BYTES of LIVE memory at addr as a hex string into out (must
 * be at least MEM_DUMP_BYTES*2+1 bytes). Only called for overlay-region
 * targets (see IsOverlayRegion) - this is real, per-byte emulator work
 * (MappedMemoryReadByteNocache), deliberately not done for every call the
 * way the cheap register snapshot already is; see the black-screen-bug
 * postmortem above PortalTraceLogCall for why extra per-call cost here would
 * be dangerous if it weren't scoped this narrowly.
 *
 * The point: tools/study_call.py's static disassembler can only read
 * whichever OL*.BIN file we point it at - it has no way to know which of
 * the 16 (extraction/OL00.BIN..OL15.BIN, none extracted yet as of
 * 2026-08-06) was actually loaded into this address range at the moment of
 * THIS specific real call. Yabause, running the real game, always knows -
 * it's just reading its own correctly-mapped memory. This dump is that
 * ground truth: a follow-up script diffs it against each extracted OL*.BIN
 * candidate at the same offset to identify which overlay was live, instead
 * of guessing. */
static void FormatMemDump(u32 addr, char *out, size_t out_size) {
  size_t pos = 0;
  out[0] = '\0';
  for (int i = 0; i < MEM_DUMP_BYTES && pos + 2 < out_size; i++) {
    u8 b = MappedMemoryReadByteNocache(addr + (u32)i);
    int n = snprintf(out + pos, out_size - pos, "%02X", b);
    if (n < 0) break;
    pos += (size_t)n;
  }
}

void PortalTraceLogCall(u32 pc, u32 target, const char *mnem) {
  if (!g_fp || !CurrentSH2) return;
  if (SkipBios() && (InBios(pc) || InBios(target))) return;
  SH2_struct *sh = CurrentSH2;
  char mem_dump[MEM_DUMP_BYTES * 2 + 1] = {0};
  if (IsOverlayRegion(target)) {
    FormatMemDump(target, mem_dump, sizeof(mem_dump));
  }
  fprintf(g_fp,
    "{\"type\":\"call\",\"frame\":%llu,\"pc\":\"%08X\",\"target\":\"%08X\",\"mnem\":\"%s\","
    "\"r0\":\"%08X\",\"r1\":\"%08X\",\"r2\":\"%08X\",\"r3\":\"%08X\","
    "\"r4\":\"%08X\",\"r5\":\"%08X\",\"r6\":\"%08X\",\"r7\":\"%08X\",\"pr\":\"%08X\","
    "\"mem_dump\":\"%s\"}\n",
    (unsigned long long)g_frame, pc, target, mnem,
    sh->regs.R[0], sh->regs.R[1], sh->regs.R[2], sh->regs.R[3],
    sh->regs.R[4], sh->regs.R[5], sh->regs.R[6], sh->regs.R[7], sh->regs.PR,
    mem_dump);
  MaybeFlush();
}

void PortalTraceReturn(void) {
  if (!g_fp || !CurrentSH2) return;
  if (SkipBios() && InBios(CurrentSH2->regs.PC)) return;
  fprintf(g_fp, "{\"type\":\"return\",\"frame\":%llu,\"pc\":\"%08X\",\"to\":\"%08X\"}\n",
    (unsigned long long)g_frame, CurrentSH2->regs.PC, CurrentSH2->regs.PR);
  MaybeFlush();
}

void PortalTraceMemWrite(u32 addr, u32 val, int width, const char *region) {
  if (!g_fp || !CurrentSH2) return;
  if (SkipBios() && InBios(CurrentSH2->regs.PC)) return;
  fprintf(g_fp,
    "{\"type\":\"mem_write\",\"frame\":%llu,\"pc\":\"%08X\",\"addr\":\"%08X\",\"val\":\"%X\",\"width\":%d,\"region\":\"%s\"}\n",
    (unsigned long long)g_frame, CurrentSH2->regs.PC, addr, val, width, region);
  MaybeFlush();
}

void PortalTraceClose(void) {
  if (g_fp) { fflush(g_fp); fclose(g_fp); g_fp = NULL; }
}

#endif
