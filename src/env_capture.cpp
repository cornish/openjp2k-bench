#include "env_capture.h"
#include "json_out.h"
#include "build_info.h"

#include <fstream>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

namespace jp2kbench {
namespace {

std::string read_first_line(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::string line;
  std::getline(f, line);
  return line;
}

std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("model name", 0) == 0) {
      auto pos = line.find(':');
      if (pos != std::string::npos) {
        std::string v = line.substr(pos + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
        return v;
      }
    }
  }
  return "unknown";
}

int cores_online() {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : (int)std::thread::hardware_concurrency();
}

std::string governor() {
  auto v = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  return v.empty() ? "unknown" : v;
}

// "1" => turbo disabled. File absent on AMD / non-intel_pstate kernels.
std::string turbo_disabled() {
  auto v = read_first_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
  if (v == "1") return "true";
  if (v == "0") return "false";
  return "\"unknown\"";
}

std::string kernel() {
  struct utsname u{};
  if (uname(&u) != 0) return "unknown";
  return std::string(u.sysname) + " " + u.release + " " + u.machine;
}

}  // namespace

std::string capture_env_json() {
  std::ostringstream os;
  os << "{";
  os << "\"cpu_model\": \"" << json_escape(cpu_model()) << "\", ";
  os << "\"cores_online\": " << cores_online() << ", ";
  os << "\"governor\": \"" << json_escape(governor()) << "\", ";
  os << "\"turbo_disabled\": " << turbo_disabled() << ", ";
  os << "\"kernel\": \"" << json_escape(kernel()) << "\", ";
  os << "\"compiler\": \"" << JP2KBENCH_COMPILER_ID << " "
                            << JP2KBENCH_COMPILER_VERSION << "\", ";
  os << "\"compile_flags\": \"" << json_escape(JP2KBENCH_COMPILE_FLAGS) << "\", ";
  os << "\"libs\": {";
  os <<   "\"openjpeg\": {"
     <<   "\"version\": \"" << json_escape(JP2KBENCH_OPENJPEG_VERSION) << "\", "
     <<   "\"commit\": \""  << json_escape(JP2KBENCH_OPENJPEG_COMMIT)  << "\"}";
  os << ", \"grok\": {"
     <<   "\"version\": \"" << json_escape(JP2KBENCH_GROK_VERSION) << "\", "
     <<   "\"commit\": \""  << json_escape(JP2KBENCH_GROK_COMMIT)  << "\"}";
  os << "}";
  os << "}";
  return os.str();
}

}  // namespace jp2kbench
