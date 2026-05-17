#pragma once
#include <cstdint>

namespace jp2kbench {

// Returns current process peak RSS in kilobytes (Linux convention; ru_maxrss
// is KB). On non-Linux platforms returns 0.
uint64_t peak_rss_kb();

}  // namespace jp2kbench
