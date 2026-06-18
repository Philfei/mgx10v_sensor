#include "camera_timestamp_assignment.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  slam::FrameTimestampTracker tracker;

  const slam::ns_t base_trigger_ns = 1'781'695'910'350'000'000LL;
  const slam::ns_t later_cam_sync_ns = base_trigger_ns + 100'000'000LL;

  const slam::ns_t ts0 = slam::assign_camera_mid_exposure_timestamp_ns(
      tracker, 1'000'000ULL, base_trigger_ns, 10'000);
  const slam::ns_t ts1 = slam::assign_camera_mid_exposure_timestamp_ns(
      tracker, 1'050'000ULL, later_cam_sync_ns, 10'000);
  const slam::ns_t ts2 = slam::assign_camera_mid_exposure_timestamp_ns(
      tracker, 1'100'000ULL, later_cam_sync_ns, 10'000);

  require(ts0 == base_trigger_ns + 5'000'000LL,
          "first frame should use cam_sync anchor plus mid exposure");
  require(ts1 - ts0 == 50'000'000LL,
          "PTS delta should prevent a false 100ms timestamp jump");
  require(ts2 - ts1 == 50'000'000LL,
          "PTS delta should prevent duplicate cam_sync timestamps");

  return 0;
}
