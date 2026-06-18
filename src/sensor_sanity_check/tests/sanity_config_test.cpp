#include "sanity_config.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void require_near(double actual, double expected, const char* message) {
  require(std::fabs(actual - expected) < 1e-9, message);
}

}  // namespace

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "sensor_sanity_config.yaml";
  {
    std::ofstream out(path);
    out << "gap_thresholds:\n"
        << "  cam_left: 0.08\n"
        << "  cam_right: 0.09\n"
        << "  imu: 0.012\n"
        << "  gnss: 0.2\n";
  }

  const auto thresholds = load_gap_thresholds(path.string());
  require_near(thresholds.at("cam_left"), 0.08, "cam_left threshold mismatch");
  require_near(thresholds.at("cam_right"), 0.09,
               "cam_right threshold mismatch");
  require_near(thresholds.at("imu"), 0.012, "imu threshold mismatch");
  require_near(thresholds.at("gnss"), 0.2, "gnss threshold mismatch");

  std::filesystem::remove(path);
  return 0;
}
