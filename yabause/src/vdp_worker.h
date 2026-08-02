/*
  Fire-and-forget / call-and-wait dispatcher moving VDP1 CPU-side draw
  preparation (command-list parsing, vertex/texture prep in Vdp1DrawCommands)
  onto its own worker thread, mirroring sound_worker.h's design exactly
  (see that header for the full rationale) but scoped to what VDP1 needs:

    - PostVoid: fire-and-forget, used for Vdp1Draw() (the CPU-side command
      parsing). The calling (SH-2) thread does not wait for this to finish.

    - Barrier: blocking, waits until every previously-Post'ed job has
      actually finished running. Must be called before touching anything
      Vdp1Draw() produces (e.g. before VIDCore->Vdp1DrawEnd() submits the
      prepared geometry via real gl* calls) or before reading state it
      writes (Vdp1Regs->COPR/EDSR, Vdp1External.status).

  IMPORTANT: unlike sound_worker.h, there is no Post/CallReadByte-style
  register dispatch here, and deliberately no attempt to move gl* calls
  themselves onto this worker thread. The core never manages its own EGL/GL
  context (confirmed: no eglMakeCurrent/eglCreateContext anywhere in this
  codebase) - every frontend it supports (libretro's hw_render callback, Qt's
  YabauseGL::makeCurrent, GLFW's glfwMakeContextCurrent) hands the context to
  exactly one thread of its own choosing, and only that thread may call gl*.
  This worker thread must never call gl* - only CPU-side parsing/prep. Actual
  GL submission (VIDCore->Vdp1DrawEnd()) stays on whichever thread already
  owns the context today.
*/
#ifndef VDP_WORKER_H
#define VDP_WORKER_H

#ifdef __cplusplus
extern "C" {
#endif

void VdpWorkerStart(void);
void VdpWorkerStop(void);

/* Fire-and-forget. */
void VdpWorkerPostVoid(void (*impl)(void));

/* Blocking barrier: waits until every previously-Post'ed job has finished
 * running, without itself doing any work. */
void VdpWorkerBarrier(void);

#ifdef __cplusplus
}
#endif

#endif /* VDP_WORKER_H */
