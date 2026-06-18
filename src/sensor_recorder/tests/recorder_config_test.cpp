#include "recorder_config.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    "sensor_recorder_config_test.yaml";
  {
    std::ofstream out(path);
    out << "data_root: /tmp/recorded\n"
        << "chunk_duration_s: 7\n"
        << "topics:\n"
        << "  - name: cam_left\n"
        << "    type: raw_image\n"
        << "    endpoint: ipc:///tmp/cam_left\n"
        << "    topic: cam_left_topic\n"
        << "    enabled: true\n"
        << "  - name: disabled_imu\n"
        << "    type: imu\n"
        << "    endpoint: ipc:///tmp/imu_data\n"
        << "    topic: imu_topic\n"
        << "    enabled: false\n"
        << "  - name: gnss\n"
        << "    type: gnss\n"
        << "    endpoint: ipc:///tmp/gnss_data\n"
        << "    topic: gnss_topic\n";
  }

  const RecorderConfig cfg = load_recorder_config(path.string());
  require(cfg.data_root == "/tmp/recorded", "data_root mismatch");
  require(cfg.chunk_duration_s == 7, "chunk_duration_s mismatch");
  require(cfg.topics.size() == 2, "enabled topic count mismatch");

  require(cfg.topics[0].name == "cam_left", "first topic name mismatch");
  require(cfg.topics[0].type == TopicType::RawImage,
          "first topic type mismatch");
  require(cfg.topics[0].endpoint == "ipc:///tmp/cam_left",
          "first topic endpoint mismatch");
  require(cfg.topics[0].topic == "cam_left_topic",
          "first topic value mismatch");

  require(cfg.topics[1].name == "gnss", "second topic name mismatch");
  require(cfg.topics[1].type == TopicType::Gnss,
          "second topic type mismatch");
  require(cfg.topics[1].endpoint == "ipc:///tmp/gnss_data",
          "second topic endpoint mismatch");
  require(cfg.topics[1].topic == "gnss_topic",
          "second topic value mismatch");

  std::filesystem::remove(path);
  return 0;
}
