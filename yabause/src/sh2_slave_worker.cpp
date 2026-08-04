/*
  Implementation of the SH-2 Slave worker dispatcher declared in
  sh2_slave_worker.h. See that header for the design rationale, and
  dispatcher.h for the underlying Post/CallVoid primitives (shared with
  sound_worker.cpp and vdp_worker.cpp).
*/
#include "sh2_slave_worker.h"
#include "dispatcher.h"
#include <mutex>

namespace {

Dispatcher g_sh2_slave_worker;

/* Guards MSH2's state specifically against the FRT input-capture
 * cross-trigger's reentrant SH2Exec(MSH2, 32) call (sh2core.c's
 * SSH2InputCaptureWriteWord, which can fire from deep inside the Slave's
 * own call stack, on the Slave worker thread) racing against the main
 * thread's own per-deciline SH2Exec(MSH2, ...) calls in yabause.c.
 *
 * IMPORTANT, and previously gotten wrong here: this must NOT wrap the
 * Slave's own ordinary execution (see Sh2SlaveWorkerPostExec below - it
 * used to take this lock for the whole impl(ctx, cycles) call, which was
 * a real bug, not a safety margin). The Slave's normal work only ever
 * touches SSH2's own state, never MSH2's - it has nothing to do with what
 * this mutex protects. Locking it for the Slave's entire execution meant
 * Master and Slave could never actually run at the same time, on
 * different cores, ever - which defeats the entire point of giving the
 * Slave its own OS thread in the first place. The only moment Slave's
 * thread needs this lock is the reentrant cross-trigger itself, which
 * already takes it locally, inside SSH2InputCaptureWriteWord in
 * sh2core.c - that is sufficient on its own.
 *
 * Recursive because the reentrant call can nest on the same thread as
 * another already-held acquisition of this same lock in some call
 * orderings - a plain non-recursive mutex would deadlock the thread
 * against itself there. */
std::recursive_mutex g_sh2_exec_mtx;

} // namespace

extern "C" {

void Sh2SlaveWorkerStart(void) { g_sh2_slave_worker.Start(); }
void Sh2SlaveWorkerStop(void) { g_sh2_slave_worker.Stop(); }

void Sh2SlaveWorkerPostExec(void (FASTCALL *impl)(SH2_struct *, u32), SH2_struct *ctx, u32 cycles) {
  /* Deliberately no lock here - see g_sh2_exec_mtx's doc comment above.
   * This is what actually lets Master and Slave run concurrently on
   * separate cores; the reentrant cross-trigger case is handled entirely
   * inside sh2core.c's SSH2InputCaptureWriteWord, not here. */
  g_sh2_slave_worker.Post([impl, ctx, cycles]() {
    impl(ctx, cycles);
  });
}

void Sh2SlaveWorkerBarrier(void) {
  g_sh2_slave_worker.CallVoid([]() {});
}

void Sh2MasterExecMutexLock(void) { g_sh2_exec_mtx.lock(); }
void Sh2MasterExecMutexUnlock(void) { g_sh2_exec_mtx.unlock(); }

} // extern "C"
