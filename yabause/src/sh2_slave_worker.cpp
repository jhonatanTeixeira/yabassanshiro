/*
  Implementation of the SH-2 Slave worker dispatcher declared in
  sh2_slave_worker.h. See that header for the design rationale, and
  dispatcher.h for the underlying Post/CallVoid primitives (shared with
  sound_worker.cpp and vdp_worker.cpp).
*/
#include "sh2_slave_worker.h"
#include "dispatcher.h"

namespace {

Dispatcher g_sh2_slave_worker;

} // namespace

extern "C" {

void Sh2SlaveWorkerStart(void) { g_sh2_slave_worker.Start(); }
void Sh2SlaveWorkerStop(void) { g_sh2_slave_worker.Stop(); }

void Sh2SlaveWorkerPostExec(void (FASTCALL *impl)(SH2_struct *, u32), SH2_struct *ctx, u32 cycles) {
  g_sh2_slave_worker.Post([impl, ctx, cycles]() { impl(ctx, cycles); });
}

void Sh2SlaveWorkerBarrier(void) {
  g_sh2_slave_worker.CallVoid([]() {});
}

} // extern "C"
