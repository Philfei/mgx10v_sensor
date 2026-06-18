#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class ReplaySensorKind {
  RawImage,
  Imu,
  Gnss,
};

struct ReplayTopic {
  std::string name;
  ReplaySensorKind kind = ReplaySensorKind::RawImage;
  std::string endpoint;
  std::string topic;
  bool enabled = true;
};

struct ReplayEvent {
  int64_t timestamp_ns = 0;
  std::string stream_name;
  ReplaySensorKind kind = ReplaySensorKind::RawImage;
  std::string endpoint;
  std::string topic;
  std::filesystem::path file_path;
  std::vector<uint8_t> header;
  uint64_t raw_offset = 0;
  uint32_t raw_size = 0;
};

std::vector<ReplayEvent> load_replay_events(
    const std::filesystem::path& dataset_dir,
    const std::vector<ReplayTopic>& topics);

std::string read_event_raw(const ReplayEvent& event);
