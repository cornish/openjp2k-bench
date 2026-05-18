#include "rss.h"

#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jp2kbench {

uint64_t peak_rss_kb() {
  struct rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
  // Linux: ru_maxrss is KB. (macOS: bytes — we don't target macOS here.)
  return (uint64_t)ru.ru_maxrss;
}

uint64_t current_rss_kb() {
  // /proc/self/status — line "VmRSS:\t<kb> kB". Cheap (~10 μs); robust
  // across glibc versions where ru_maxrss has its own quirks.
  std::FILE* fp = std::fopen("/proc/self/status", "r");
  if (!fp) return 0;
  char line[256];
  uint64_t kb = 0;
  while (std::fgets(line, sizeof(line), fp)) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      kb = (uint64_t)std::strtoull(line + 6, nullptr, 10);
      break;
    }
  }
  std::fclose(fp);
  return kb;
}

void SampledRssPeak::start() {
  if (running_.exchange(true)) return;
  peak_kb_.store(0);
  thr_ = std::thread([this] {
    // Sample at period_ms_ until running_ flips. The sampler thread runs
    // alongside the decode; on a multi-core box this is essentially free
    // (one syscall + line parse per period). The bench's decode threads
    // pin compute on their own cores.
    while (running_.load(std::memory_order_relaxed)) {
      uint64_t kb = current_rss_kb();
      uint64_t prev = peak_kb_.load(std::memory_order_relaxed);
      while (kb > prev && !peak_kb_.compare_exchange_weak(prev, kb)) {}
      std::this_thread::sleep_for(std::chrono::milliseconds(period_ms_));
    }
  });
}

uint64_t SampledRssPeak::stop() {
  if (running_.exchange(false)) {
    if (thr_.joinable()) thr_.join();
    // Take one last sample after the join — catches a peak that landed
    // between the last poll and the sentinel flip.
    uint64_t kb = current_rss_kb();
    uint64_t prev = peak_kb_.load(std::memory_order_relaxed);
    while (kb > prev && !peak_kb_.compare_exchange_weak(prev, kb)) {}
  }
  return peak_kb_.load();
}

}  // namespace jp2kbench
