/*
  Implementation of the generic sound worker dispatcher declared in
  sound_worker.h. See that header for the design rationale.
*/
#include "sound_worker.h"

#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace {

class SoundWorker {
public:
  void Start() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&SoundWorker::Run, this);
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (!running_) return;
      running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  bool OnWorkerThread() const {
    return running_ && std::this_thread::get_id() == thread_id_;
  }

  /* Fire-and-forget. */
  void Post(std::function<void()> job) {
    if (OnWorkerThread()) { job(); return; }
    {
      std::lock_guard<std::mutex> lk(mtx_);
      queue_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

  /* Blocking, no result. */
  void CallVoid(const std::function<void()>& job) {
    if (OnWorkerThread()) { job(); return; }
    std::promise<void> done;
    std::future<void> fut = done.get_future();
    Post([&job, &done]() {
      job();
      done.set_value();
    });
    fut.wait();
  }

  /* Blocking, with result. */
  template <typename T>
  T CallValue(const std::function<T()>& job) {
    if (OnWorkerThread()) return job();
    std::promise<T> result;
    std::future<T> fut = result.get_future();
    Post([&job, &result]() {
      result.set_value(job());
    });
    return fut.get();
  }

private:
  void Run() {
    thread_id_ = std::this_thread::get_id();
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this]() { return !running_ || !queue_.empty(); });
        if (!running_ && queue_.empty()) return;
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      job();
    }
  }

  std::thread thread_;
  std::thread::id thread_id_;
  std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool running_ = false;
};

SoundWorker g_worker;

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
