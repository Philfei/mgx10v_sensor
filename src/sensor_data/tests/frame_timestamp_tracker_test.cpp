#include "frame_timestamp_tracker.h"

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

  const slam::ns_t base_ns = 1'781'695'910'350'000'000LL;
  const slam::ns_t next_sync_ns = base_ns + 100'000'000LL;

  const slam::ns_t ts0 = tracker.assign(1'000'000ULL, base_ns);
  const slam::ns_t ts1 = tracker.assign(1'050'000ULL, next_sync_ns);
  const slam::ns_t ts2 = tracker.assign(1'100'000ULL, next_sync_ns);

  require(ts0 == base_ns, "first frame should use cam_sync anchor");
  require(ts1 - ts0 == 50'000'000LL,
          "PTS delta should prevent 100ms timestamp jump");
  require(ts2 - ts1 == 50'000'000LL,
          "PTS delta should prevent duplicated timestamp");

  const slam::ns_t ts3 = tracker.assign(1'200'000ULL, next_sync_ns);
  require(ts3 - ts2 == 100'000'000LL,
          "real PTS gaps should remain visible as timestamp gaps");

  return 0;
}
