/*
  Fire-and-forget / call-and-wait dispatcher moving the SH-2 Slave core's
  SH2Exec() calls onto its own worker thread, mirroring vdp_worker.h's design
  exactly (see that header for the full rationale) but scoped to the Slave:

    - PostExec: fire-and-forget, used for SH2Exec(SSH2, cycles). The calling
      (main/Master) thread does not wait for this to finish on its own -
      every call site in yabause.c pairs this with an immediate Barrier
      (see below), never a deferred one, matching the pattern already
      validated for VDP1 (a deferred barrier was tried for VDP2 and crashed).

    - Barrier: blocking, waits until every previously-Post'ed job has
      actually finished running. Called immediately after PostExec at every
      call site, so from the Master's point of view SH2Exec(SSH2, cycles)
      still behaves as a synchronous call with the exact same cycle-accurate
      timing as today - only the physical thread it runs on changes.

  Why an immediate (not deferred) barrier is safe here despite adding no
  real algorithmic parallelism: the Master and Slave SH-2 cores are already
  required to never advance the "same simulated instant" concurrently (see
  the dynarec thread-safety work in sh2_dynarec_devmiyax/ - CurrentContext
  is thread_local, CompileBlocks::cache_mtx_ guards the shared JIT block
  cache), so there's no correctness case where letting the Master run ahead
  of the Slave would help. The benefit is instead that the Master and Slave
  stop time-slicing the SAME physical CPU core's L1i/L1d/branch predictor -
  moving the Slave to its own OS thread lets the scheduler place it on a
  different core, which is a real (if modest, unmeasured without a device)
  win on weak multi-core parts like the RK3326, independent of parallelism.

  The FRT Input Capture cross-trigger (sh2core.c's SSH2InputCaptureWriteWord,
  which can reentrantly call SH2Exec(MSH2, 32) from deep inside the Slave's
  own execution) needs no special handling from this dispatcher: the Master
  thread is fully parked in Barrier() for the whole duration of the Slave's
  PostExec job, including any such reentrant call, so there is never
  concurrent access to MSH2's state. See DynarecSh2CInterface.cpp's
  SH2DynExec for the one real bug that investigation found (a stale
  CurrentContext left behind by a reentrant call, now fixed with the same
  save/restore idiom sh2core.c already used for CurrentSH2).

  As with the VDP1 worker: no gl* calls happen here, and none should ever be
  added - this is CPU-only SH-2 core execution.
*/
#ifndef SH2_SLAVE_WORKER_H
#define SH2_SLAVE_WORKER_H

#include "sh2core.h"

#ifdef __cplusplus
extern "C" {
#endif

void Sh2SlaveWorkerStart(void);
void Sh2SlaveWorkerStop(void);

/* Fire-and-forget: runs impl(ctx, cycles) on the worker thread. */
void Sh2SlaveWorkerPostExec(void (FASTCALL *impl)(SH2_struct *, u32), SH2_struct *ctx, u32 cycles);

/* Blocking barrier: waits until every previously-Post'ed job has finished
 * running, without itself doing any work. */
void Sh2SlaveWorkerBarrier(void);

#ifdef __cplusplus
}
#endif

#endif /* SH2_SLAVE_WORKER_H */
