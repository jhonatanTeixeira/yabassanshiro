/*
  Implementation of the VDP1 worker dispatcher declared in vdp_worker.h.
  See that header for the design rationale, and dispatcher.h for the
  underlying Post/CallVoid primitives (shared with sound_worker.cpp).
*/
#include "vdp_worker.h"
#include "dispatcher.h"

namespace {

Dispatcher g_vdp_worker;

} // namespace

extern "C" {

void VdpWorkerStart(void) { g_vdp_worker.Start(); }
void VdpWorkerStop(void) { g_vdp_worker.Stop(); }

void VdpWorkerPostVoid(void (*impl)(void)) {
  g_vdp_worker.Post([impl]() { impl(); });
}

void VdpWorkerBarrier(void) {
  g_vdp_worker.CallVoid([]() {});
}

} // extern "C"
