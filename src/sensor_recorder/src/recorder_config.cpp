#include "recorder_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace {

std::string trim(std::string value) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char c) { return !is_space(c); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char c) { return !is_space(c); })
                  .base(),
              value.end());
  return value;
}

std::string strip_comment(const std::string& line) {
  bool in_quote = false;
  char quote = '\0';
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if ((c == '\'' || c == '"') && (i == 0 || line[i - 1] != '\\')) {
      if (!in_quote) {
        in_quote = true;
        quote = c;
      } else if (quote == c) {
        in_quote = false;
      }
    }
    if (c == '#' && !in_quote) {
      return line.substr(0, i);
    }
  }
  return line;
}

std::string unquote(std::string value) {
  value = trim(std::move(value));
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') ||
       (value.front() == '\'' && value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

bool parse_bool(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower == "true" || lower == "yes" || lower == "1" || lower == "on";
}

TopicType parse_topic_type(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "raw_image" || lower == "image" || lower == "camera") {
    return TopicType::RawImage;
  }
  if (lower == "imu") {
    return TopicType::Imu;
  }
  if (lower == "gnss" || lower == "rtk") {
    return TopicType::Gnss;
  }
  throw std::runtime_error("unknown topic type: " + value);
}

bool split_key_value(const std::string& line, std::string* key,
                     std::string* value) {
  const size_t pos = line.find(':');
  if (pos == std::string::npos) {
    return false;
  }
  *key = trim(line.substr(0, pos));
  *value = unquote(line.substr(pos + 1));
  return !key->empty();
}

void validate_topic(const TopicConfig& topic) {
  if (topic.name.empty()) {
    throw std::runtime_error("topic entry is missing name");
  }
  if (topic.endpoint.empty()) {
    throw std::runtime_error("topic " + topic.name + " is missing endpoint");
  }
  if (topic.topic.empty()) {
    throw std::runtime_error("topic " + topic.name + " is missing topic");
  }
}

}  // namespace

RecorderConfig load_recorder_config(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("failed to open config: " + path);
  }

  RecorderConfig config;
  TopicConfig current;
  bool in_topics = false;
  bool have_current = false;
  bool current_enabled = true;

  auto finish_topic = [&]() {
    if (!have_current) {
      return;
    }
    validate_topic(current);
    if (current_enabled) {
      config.topics.push_back(current);
    }
    current = TopicConfig{};
    current_enabled = true;
    have_current = false;
  };

  std::string raw_line;
  while (std::getline(in, raw_line)) {
    std::string line = trim(strip_comment(raw_line));
    if (line.empty()) {
      continue;
    }

    if (line == "topics:") {
      in_topics = true;
      continue;
    }

    if (in_topics && line.rfind("- ", 0) == 0) {
      finish_topic();
      have_current = true;
      line = trim(line.substr(2));
      if (line.empty()) {
        continue;
      }
    }

    std::string key;
    std::string value;
    if (!split_key_value(line, &key, &value)) {
      throw std::runtime_error("invalid config line: " + raw_line);
    }

    if (!in_topics) {
      if (key == "data_root") {
        config.data_root = value;
      } else if (key == "chunk_duration_s") {
        config.chunk_duration_s = std::max(1, std::stoi(value));
      }
      continue;
    }

    have_current = true;
    if (key == "name") {
      current.name = value;
    } else if (key == "type") {
      current.type = parse_topic_type(value);
    } else if (key == "endpoint") {
      current.endpoint = value;
    } else if (key == "topic") {
      current.topic = value;
    } else if (key == "enabled") {
      current_enabled = parse_bool(value);
    }
  }
  finish_topic();

  if (config.topics.empty()) {
    throw std::runtime_error("config contains no enabled topics");
  }
  return config;
}

const char* topic_type_name(TopicType type) {
  switch (type) {
    case TopicType::RawImage:
      return "raw_image";
    case TopicType::Imu:
      return "imu";
    case TopicType::Gnss:
      return "gnss";
  }
  return "unknown";
}
