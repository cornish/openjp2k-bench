#pragma once
#include <string>

namespace jp2kbench {

// Returns a complete JSON object string ("{...}") describing host + build
// environment. Designed to be embedded into RunHeader.env_json verbatim.
std::string capture_env_json();

}  // namespace jp2kbench
