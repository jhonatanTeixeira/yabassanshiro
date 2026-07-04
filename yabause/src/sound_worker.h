/*
  Generic fire-and-forget / call-and-wait dispatcher used to move SCSP/M68K
  sound processing onto its own worker thread while keeping every call site
  in scsp.c looking like a plain function call.

  Two dispatch flavors, matching how the caller needs the result:

    - "Post"  variants enqueue work on the worker thread and return
      immediately (fire-and-forget). Used when the caller doesn't need a
      return value and doesn't need to block on completion -- e.g. stepping
      the M68K for N cycles, or writing an SCSP register.

    - "Call"  variants enqueue work and then block until it has actually run
      on the worker thread, returning its result if any (implemented with a
      promise/future under the hood). Used when the caller needs a value
      back -- e.g. reading an SCSP register -- or needs a barrier guaranteeing
      all previously-posted work has drained (e.g. before pulling a finished
      audio buffer).

  All entry points are safe to call from the worker thread itself (e.g. the
  M68K core touching its own SCSP registers): if the caller is already
  running on the worker thread, the implementation is invoked directly
  in-place instead of being re-enqueued, avoiding self-deadlock.

  Ordering guarantee: the worker thread is a single consumer draining a FIFO
  queue, so anything Post'ed before a later Post/Call is guaranteed to have
  already run by the time that later call's implementation executes -- this
  is what makes fire-and-forget writes safe to interleave with blocking
  reads without any explicit locking in scsp.c itself.
*/
#ifndef SOUND_WORKER_H
#define SOUND_WORKER_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

void SoundWorkerStart(void);
void SoundWorkerStop(void);

/* Fire-and-forget. */
void SoundWorkerPostExec(void (*impl)(s32), s32 cycles);
void SoundWorkerPostVoid(void (*impl)(void));
void SoundWorkerPostWriteByte(u32 addr, u8  val, void (*impl)(u32, u8));
void SoundWorkerPostWriteWord(u32 addr, u16 val, void (*impl)(u32, u16));
void SoundWorkerPostWriteLong(u32 addr, u32 val, void (*impl)(u32, u32));

/* Blocking: waits for the result / for the job to actually finish. */
u8   SoundWorkerCallReadByte(u32 addr, u8  (*impl)(u32));
u16  SoundWorkerCallReadWord(u32 addr, u16 (*impl)(u32));
u32  SoundWorkerCallReadLong(u32 addr, u32 (*impl)(u32));

/* Blocking barrier: waits until every previously-Post'ed job has finished
 * running, without itself doing any work. Use before reading state (e.g. a
 * mixed audio buffer) that the worker thread produces as a side effect of
 * jobs posted earlier. */
void SoundWorkerBarrier(void);

#ifdef __cplusplus
}
#endif

#endif /* SOUND_WORKER_H */
