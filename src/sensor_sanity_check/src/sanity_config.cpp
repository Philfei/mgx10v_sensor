#include "sanity_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace {

std::string trim(std::string value) {
  auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string strip_comment(const std::string& line) {
  const size_t pos = line.find('#');
  return pos == std::string::npos ? line : line.substr(0, pos);
}

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

}  // namespace

std::unordered_map<std::string, double> load_gap_thresholds(
    const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("cannot open config: " + path);
  }

  std::unordered_map<std::string, double> thresholds;
  bool in_thresholds = false;
  std::string raw_line;
  while (std::getline(in, raw_line)) {
    const std::string line = trim(strip_comment(raw_line));
    if (line.empty()) {
      continue;
    }
    if (line == "gap_thresholds:") {
      in_thresholds = true;
      continue;
    }
    if (!in_thresholds) {
      continue;
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = to_lower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    if (!key.empty() && !value.empty()) {
      thresholds[key] = std::stod(value);
    }
  }
  return thresholds;
}
