#include "rss.h"

#include <sys/resource.h>

namespace jp2kbench {

uint64_t peak_rss_kb() {
  struct rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
  // Linux: ru_maxrss is KB. (macOS: bytes — we don't target macOS here.)
  return (uint64_t)ru.ru_maxrss;
}

}  // namespace jp2kbench
