/*
  Implementation of the generic sound worker dispatcher declared in
  sound_worker.h. See that header for the design rationale, and
  dispatcher.h for the underlying Post/CallVoid/CallValue primitives
  (shared with vdp_worker.cpp - extracted here so both workers use the
  exact same tested synchronization logic instead of duplicating it).
*/
#include "sound_worker.h"
#include "dispatcher.h"

namespace {

Dispatcher g_worker;

} // namespace

extern "C" {

void SoundWorkerStart(void) { g_worker.Start(); }
void SoundWorkerStop(void) { g_worker.Stop(); }

void SoundWorkerPostExec(void (*impl)(s32), s32 cycles) {
  g_worker.Post([impl, cycles]() { impl(cycles); });
}

void SoundWorkerPostVoid(void (*impl)(void)) {
  g_worker.Post([impl]() { impl(); });
}

void SoundWorkerPostWriteByte(u32 addr, u8 val, void (*impl)(u32, u8)) {
  g_worker.Post([impl, addr, val]() { impl(addr, val); });
}

void SoundWorkerPostWriteWord(u32 addr, u16 val, void (*impl)(u32, u16)) {
  g_worker.Post([impl, addr, val]() { impl(addr, val); });
}

void SoundWorkerPostWriteLong(u32 addr, u32 val, void (*impl)(u32, u32)) {
  g_worker.Post([impl, addr, val]() { impl(addr, val); });
}

u8 SoundWorkerCallReadByte(u32 addr, u8 (*impl)(u32)) {
  std::function<u8()> job = [impl, addr]() { return impl(addr); };
  return g_worker.CallValue<u8>(job);
}

u16 SoundWorkerCallReadWord(u32 addr, u16 (*impl)(u32)) {
  std::function<u16()> job = [impl, addr]() { return impl(addr); };
  return g_worker.CallValue<u16>(job);
}

u32 SoundWorkerCallReadLong(u32 addr, u32 (*impl)(u32)) {
  std::function<u32()> job = [impl, addr]() { return impl(addr); };
  return g_worker.CallValue<u32>(job);
}

void SoundWorkerBarrier(void) {
  g_worker.CallVoid([]() {});
}

} // extern "C"
