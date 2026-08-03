/*
  Fire-and-forget / call-and-wait dispatcher moving the SH-2 Slave core's
  SH2Exec() calls onto its own worker thread, mirroring vdp_worker.h's design
  but scoped to the Slave. Two generations of this design were tried before
  landing on the current one - both failed in testing, and the reasons why
  matter for anyone touching this file:

    - PostExec: fire-and-forget - the calling (main/Master) thread does not
      wait for this to finish. First tried paired with an IMMEDIATE Barrier
      at every call site (matching VDP1's pattern): this was safe but
      measurably regressed both CPU usage and framerate once tested against
      a game that actually drives the Slave, because the call site is the
      innermost deciline loop in yabause.c - up to ~2600 times a frame, far
      more often than VDP1's per-draw-command granularity, so the full
      mutex/condvar/context-switch round trip dominated the actual work
      being dispatched.

    - Barrier: blocking, waits until every previously-Post'ed job has
      finished. Now called once per SCANLINE (yabause.c, next to
      SoundWorkerBarrier()) instead of once per deciline, matching the
      audio worker's already-proven granularity (M68KExec/M68KSync there
      are also fire-and-forget Posts, reconciled once per line via
      SoundWorkerBarrier()). NOTE: guarded by `if (yabsys.IsSSH2Running)` at
      its call site - this dispatcher is only Start()'ed inside
      YabauseStartSlave() (not unconditionally like the DSP worker), so an
      unguarded Barrier() call before that ever happens queues a wait-job
      with no worker thread to run it and blocks forever. Most games begin
      with the Slave off, so this hung on literally the first scanline.

  Batching the Barrier reopened the exact hazard the immediate-barrier
  design existed to prevent: with Master's own per-deciline SH2Exec(MSH2,..)
  calls no longer parked behind a Barrier while the Slave's queued job runs,
  the two now execute for real, concurrently in wall-clock time, for up to
  ~9 decilines before reconciling. That includes the Slave's FRT input-
  capture cross-trigger (sh2core.c's SSH2InputCaptureWriteWord), which can
  reentrantly call SH2Exec(MSH2, 32) from deep inside the Slave's own call
  stack - now a genuine data race against Master's own concurrent MSH2
  access, not a benign timing curiosity. This produced both a deadlock-
  looking hang and a livelock-looking hang in testing (same race, different
  interleaving each time).

  Fix: Sh2MasterExecMutexLock/Unlock (recursive - the reentrant call must be
  able to re-enter on the same thread without deadlocking itself) wraps the
  actual EXECUTION of both MSH2 and SSH2, not the dispatch/queueing. This
  keeps Post() cheap and lock-free, and the lock itself is normally
  uncontended (the Slave's small per-deciline cycle budget usually finishes
  well before Master needs MSH2 again) - so it does NOT reintroduce the
  round-trip cost the immediate-barrier design paid, while still making
  true concurrent execution of Master and Slave impossible. See
  sh2_slave_worker.cpp for where it's applied, and sh2core.c's
  SSH2InputCaptureWriteWord for the reentrant call sites that also need it.

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

/* Must bracket every direct SH2Exec(MSH2, ...) call made from OUTSIDE this
 * dispatcher (i.e. the main thread's own per-deciline Master execution in
 * yabause.c, and the FRT cross-trigger's reentrant calls in sh2core.c) -
 * see this header's top comment for why. Recursive: safe to re-enter on
 * the same thread (the reentrant cross-trigger case). */
void Sh2MasterExecMutexLock(void);
void Sh2MasterExecMutexUnlock(void);

#ifdef __cplusplus
}
#endif

#endif /* SH2_SLAVE_WORKER_H */
