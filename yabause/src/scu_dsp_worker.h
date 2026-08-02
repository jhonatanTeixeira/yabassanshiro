/*
  Fire-and-forget / call-and-wait dispatcher moving the SCU DSP interpreter
  loop onto its own worker thread, mirroring vdp_worker.h/sh2_slave_worker.h's
  design exactly (see those headers for the full rationale) but scoped to
  the DSP:

    - PostExec: fire-and-forget, used for ScuDspRunLoop(timing) (scu.c) -
      the DSP instruction-interpreter loop, entered only while
      ScuDsp->ProgControlPort.part.EX is set. The system-timer/DMA work in
      ScuExec() (ScuTimer1Exec, ScuDmaProc) is NOT part of this - it stays
      inline on the main thread, unchanged.

    - Barrier: blocking, waits until every previously-Post'ed job has
      actually finished running. Called immediately after PostExec at its
      one call site in scu.c, so ScuExec() still behaves as a synchronous,
      cycle-accurate call from the caller's point of view - only the
      physical thread the DSP loop runs on changes. Same immediate-barrier
      rationale as vdp_worker.h/sh2_slave_worker.h: no correctness case
      benefits from letting the caller run ahead of the DSP, so there's no
      reason to risk it; the win is purely in not time-slicing the DSP
      interpreter against the main thread's own code on the same core.

  Thread-safety note distinct from the SH-2 Slave worker: the DSP's DMA
  instructions (dsp_dma01..04 in scu.c, dispatched via step_dsp_dma) read
  and write shared system WRAM directly through MappedMemoryReadLong/
  WriteLong - the same High/Low memory the SH-2 Master (and, if enabled,
  Slave) can be executing against concurrently on other threads. This is
  deliberately left WITHOUT a lock, for the same reason the SCSP's SoundRam
  access was left unlocked: these accesses are aligned (atomic in practice
  on ARM64/x86_64), the DMA runs in short bursts rather than a continuous
  stream, and a real race window is narrow and has not shown up in testing.
  This is a conscious tradeoff, not an oversight - do not silently "fix" it
  by adding a lock without re-measuring the performance impact first (see
  the SCSP g_soundram_region lock regression earlier in this project's
  history for why a hot-path lock here could hurt more than it helps).

  As with the other workers: no gl* calls happen here, and none should ever
  be added - this is CPU-only DSP interpretation.
*/
#ifndef SCU_DSP_WORKER_H
#define SCU_DSP_WORKER_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

void ScuDspWorkerStart(void);
void ScuDspWorkerStop(void);

/* Fire-and-forget: runs impl(timing) on the worker thread. */
void ScuDspWorkerPostExec(void (*impl)(u32), u32 timing);

/* Blocking barrier: waits until every previously-Post'ed job has finished
 * running, without itself doing any work. */
void ScuDspWorkerBarrier(void);

#ifdef __cplusplus
}
#endif

#endif /* SCU_DSP_WORKER_H */
