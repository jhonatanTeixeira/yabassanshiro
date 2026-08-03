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

/* Guards actual SH2Exec() EXECUTION for both MSH2 and SSH2 - not the
 * dispatch/queueing, which stays cheap and lock-free (Dispatcher's own
 * mutex+condvar). Since Sh2SlaveWorkerPostExec() is fire-and-forget
 * (batched, reconciled only once per scanline via Sh2SlaveWorkerBarrier()),
 * the main thread's own per-deciline SH2Exec(MSH2, ...) calls now run for
 * real, concurrently in wall-clock time, with whatever the Slave worker
 * thread is executing - including the FRT input-capture cross-trigger's
 * reentrant SH2Exec(MSH2, 32) call (sh2core.c's SSH2InputCaptureWriteWord),
 * which can fire from deep inside the Slave's own call stack on this same
 * worker thread. Without this lock, that's a genuine data race on MSH2's
 * register/control state (not just a benign timing curiosity) - it's what
 * produced both a deadlock-looking hang and a livelock-looking hang in
 * testing, same underlying race, different interleaving each time.
 *
 * Recursive because the reentrant call needs to nest correctly: this
 * worker thread already holds the lock for the OUTER SH2Exec(SSH2, ...)
 * call (see Sh2SlaveWorkerPostExec below) when the INNER, same-thread
 * SH2Exec(MSH2, 32) reentrant call needs to acquire it too - a plain
 * non-recursive mutex would deadlock the thread against itself there.
 *
 * This does NOT reintroduce the immediate-barrier-per-deciline cost: the
 * lock is only actually contended in the rare case where the main thread's
 * next Master step genuinely catches up to a still-running Slave job: most
 * of the time it's uncontended (Slave finishes its small cycle budget well
 * before Master needs MSH2 again), so this is a cheap futex fast-path, not
 * a full mutex+condvar+context-switch dispatcher round trip. */
std::recursive_mutex g_sh2_exec_mtx;

} // namespace

extern "C" {

void Sh2SlaveWorkerStart(void) { g_sh2_slave_worker.Start(); }
void Sh2SlaveWorkerStop(void) { g_sh2_slave_worker.Stop(); }

void Sh2SlaveWorkerPostExec(void (FASTCALL *impl)(SH2_struct *, u32), SH2_struct *ctx, u32 cycles) {
  g_sh2_slave_worker.Post([impl, ctx, cycles]() {
    std::lock_guard<std::recursive_mutex> lk(g_sh2_exec_mtx);
    impl(ctx, cycles);
  });
}

void Sh2SlaveWorkerBarrier(void) {
  g_sh2_slave_worker.CallVoid([]() {});
}

void Sh2MasterExecMutexLock(void) { g_sh2_exec_mtx.lock(); }
void Sh2MasterExecMutexUnlock(void) { g_sh2_exec_mtx.unlock(); }

} // extern "C"
