#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace jp2kbench {

// Returns current process peak RSS in kilobytes (Linux convention; ru_maxrss
// is KB). On non-Linux platforms returns 0.
uint64_t peak_rss_kb();

// Reads current (not peak) RSS in KB from /proc/self/status's VmRSS line.
// Returns 0 on Linux if the file can't be read; returns 0 on non-Linux.
// Used by SampledRssPeak; exposed for unit testing.
uint64_t current_rss_kb();

// Polls /proc/self/status every period_ms while running. Tracks the
// maximum VmRSS observed. Used in scale-track mode to catch mid-decode
// peaks that getrusage() understates when glibc has trimmed the heap by
// the time the process exits.
//
// Usage:
//   {
//     SampledRssPeak sampler(100);
//     sampler.start();
//     do_work();
//     uint64_t peak_kb = sampler.stop();
//   }
class SampledRssPeak {
 public:
  explicit SampledRssPeak(int period_ms) : period_ms_(period_ms) {}
  ~SampledRssPeak() { stop(); }
  SampledRssPeak(const SampledRssPeak&) = delete;
  SampledRssPeak& operator=(const SampledRssPeak&) = delete;

  // Start sampling. Safe to call only once per instance.
  void start();
  // Stop sampling and return the peak observed in KB. Safe to call
  // multiple times — the second call returns the same value.
  uint64_t stop();

 private:
  int period_ms_ = 100;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> peak_kb_{0};
  std::thread thr_;
};

}  // namespace jp2kbench
