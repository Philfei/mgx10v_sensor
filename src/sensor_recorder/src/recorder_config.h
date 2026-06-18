#pragma once

#include <string>
#include <vector>

enum class TopicType {
  RawImage,
  Imu,
  Gnss,
};

struct TopicConfig {
  std::string name;
  TopicType type = TopicType::RawImage;
  std::string endpoint;
  std::string topic;
};

struct RecorderConfig {
  std::string data_root = "/root/sensor_receiver/data";
  int chunk_duration_s = 5;
  std::vector<TopicConfig> topics;
};

RecorderConfig load_recorder_config(const std::string& path);
const char* topic_type_name(TopicType type);
