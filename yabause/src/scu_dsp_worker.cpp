/*
  Implementation of the SCU DSP worker dispatcher declared in
  scu_dsp_worker.h. See that header for the design rationale, and
  dispatcher.h for the underlying Post/CallVoid primitives (shared with
  sound_worker.cpp, vdp_worker.cpp and sh2_slave_worker.cpp).
*/
#include "scu_dsp_worker.h"
#include "dispatcher.h"

namespace {

Dispatcher g_scu_dsp_worker;

} // namespace

extern "C" {

void ScuDspWorkerStart(void) { g_scu_dsp_worker.Start(); }
void ScuDspWorkerStop(void) { g_scu_dsp_worker.Stop(); }

void ScuDspWorkerPostExec(void (*impl)(u32), u32 timing) {
  g_scu_dsp_worker.Post([impl, timing]() { impl(timing); });
}

void ScuDspWorkerBarrier(void) {
  g_scu_dsp_worker.CallVoid([]() {});
}

} // extern "C"
