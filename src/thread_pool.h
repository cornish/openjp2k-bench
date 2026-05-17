#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace jp2kbench {

class ThreadPool {
 public:
  explicit ThreadPool(int n) : stop_(false) {
    if (n < 1) n = 1;
    for (int i = 0; i < n; ++i) {
      workers_.emplace_back([this] { this->worker_loop(); });
    }
  }
  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
  }

  void submit(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lk(m_);
      q_.push(std::move(fn));
    }
    cv_.notify_one();
  }

  void wait_idle() {
    std::unique_lock<std::mutex> lk(m_);
    done_cv_.wait(lk, [this] {
      return q_.empty() && active_ == 0;
    });
  }

 private:
  void worker_loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
        if (stop_ && q_.empty()) return;
        job = std::move(q_.front());
        q_.pop();
        ++active_;
      }
      job();
      {
        std::lock_guard<std::mutex> lk(m_);
        --active_;
      }
      done_cv_.notify_all();
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> q_;
  std::mutex m_;
  std::condition_variable cv_;
  std::condition_variable done_cv_;
  bool stop_;
  int active_ = 0;
};

}  // namespace jp2kbench
