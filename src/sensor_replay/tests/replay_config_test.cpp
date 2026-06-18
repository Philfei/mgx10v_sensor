#include "replay_config.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "sensor_replay_config.yaml";
  {
    std::ofstream out(path);
    out << "topics:\n"
        << "  - name: cam_left\n"
        << "    type: raw_image\n"
        << "    endpoint: ipc:///tmp/custom_left\n"
        << "    topic: custom_left_topic\n"
        << "    enabled: true\n"
        << "  - name: imu\n"
        << "    type: imu\n"
        << "    endpoint: ipc:///tmp/imu_data\n"
        << "    topic: imu_topic\n"
        << "    enabled: false\n";
  }

  const ReplayConfig cfg = load_replay_config(path.string());
  require(cfg.topics.size() == 1, "enabled topic count mismatch");
  require(cfg.topics[0].name == "cam_left", "topic name mismatch");
  require(cfg.topics[0].kind == ReplaySensorKind::RawImage,
          "topic type mismatch");
  require(cfg.topics[0].endpoint == "ipc:///tmp/custom_left",
          "endpoint mismatch");
  require(cfg.topics[0].topic == "custom_left_topic", "topic mismatch");

  std::filesystem::remove(path);
  return 0;
}
