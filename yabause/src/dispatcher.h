/*
  Generic fire-and-forget / call-and-wait single-worker-thread dispatcher.

  Extracted from the SCSP/M68K audio fix (originally private to
  sound_worker.cpp) so the same Post/CallVoid/CallValue primitives can back
  other single-producer/single-consumer worker threads (e.g. a VDP1 CPU-prep
  worker, see vdp_worker.cpp) without duplicating the synchronization logic.

  All waiting here is condition_variable::wait(lock, predicate) - a real
  OS-level suspend, never a spin/poll loop. See sound_worker.h for the
  Post/Call design rationale (fire-and-forget vs blocking, in-place
  execution when already on the worker thread to avoid self-deadlock,
  FIFO ordering guarantee).
*/
#ifndef DISPATCHER_H
#define DISPATCHER_H

#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <condition_variable>
#include <thread>

class Dispatcher {
public:
  void Start() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&Dispatcher::Run, this);
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

#endif /* DISPATCHER_H */
