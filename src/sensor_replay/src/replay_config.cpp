#include "replay_config.h"

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

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool parse_bool(const std::string& value) {
  const std::string v = lower(value);
  return v == "true" || v == "1" || v == "yes" || v == "on";
}

ReplaySensorKind parse_kind(const std::string& value) {
  const std::string v = lower(value);
  if (v == "raw_image" || v == "camera" || v == "image") {
    return ReplaySensorKind::RawImage;
  }
  if (v == "imu") {
    return ReplaySensorKind::Imu;
  }
  if (v == "gnss" || v == "rtk") {
    return ReplaySensorKind::Gnss;
  }
  throw std::runtime_error("unknown topic type: " + value);
}

bool split_key_value(const std::string& line, std::string* key,
                     std::string* value) {
  const size_t pos = line.find(':');
  if (pos == std::string::npos) return false;
  *key = trim(line.substr(0, pos));
  *value = trim(line.substr(pos + 1));
  return !key->empty();
}

void validate_topic(const ReplayTopic& topic) {
  if (topic.name.empty()) {
    throw std::runtime_error("topic is missing name");
  }
  if (topic.endpoint.empty()) {
    throw std::runtime_error("topic " + topic.name + " is missing endpoint");
  }
  if (topic.topic.empty()) {
    throw std::runtime_error("topic " + topic.name + " is missing topic");
  }
}

}  // namespace

ReplayConfig default_replay_config() {
  return {std::vector<ReplayTopic>{
      {"cam_left", ReplaySensorKind::RawImage, "ipc:///tmp/cam_left",
       "cam_left_topic", true},
      {"cam_right", ReplaySensorKind::RawImage, "ipc:///tmp/cam_right",
       "cam_right_topic", true},
      {"imu", ReplaySensorKind::Imu, "ipc:///tmp/imu_data", "imu_topic",
       true},
      {"gnss", ReplaySensorKind::Gnss, "ipc:///tmp/gnss_data", "gnss_topic",
       true},
  }};
}

ReplayConfig load_replay_config(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return default_replay_config();
  }

  ReplayConfig cfg;
  ReplayTopic current;
  bool in_topics = false;
  bool have_current = false;
  bool enabled = true;

  auto finish_topic = [&]() {
    if (!have_current) return;
    validate_topic(current);
    current.enabled = enabled;
    if (enabled) cfg.topics.push_back(current);
    current = ReplayTopic{};
    enabled = true;
    have_current = false;
  };

  std::string raw_line;
  while (std::getline(in, raw_line)) {
    std::string line = trim(strip_comment(raw_line));
    if (line.empty()) continue;
    if (line == "topics:") {
      in_topics = true;
      continue;
    }
    if (in_topics && line.rfind("- ", 0) == 0) {
      finish_topic();
      have_current = true;
      line = trim(line.substr(2));
      if (line.empty()) continue;
    }

    std::string key;
    std::string value;
    if (!split_key_value(line, &key, &value)) continue;
    if (!in_topics) continue;
    have_current = true;
    if (key == "name") {
      current.name = value;
    } else if (key == "type") {
      current.kind = parse_kind(value);
    } else if (key == "endpoint") {
      current.endpoint = value;
    } else if (key == "topic") {
      current.topic = value;
    } else if (key == "enabled") {
      enabled = parse_bool(value);
    }
  }
  finish_topic();
  if (cfg.topics.empty()) {
    return default_replay_config();
  }
  return cfg;
}
