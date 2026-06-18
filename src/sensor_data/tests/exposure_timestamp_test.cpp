#include "exposure_timestamp.h"

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
  const slam::ns_t trigger_ns = 1'000'000'000LL;

  require(slam::exposure_midpoint_timestamp_ns(trigger_ns, 0) == trigger_ns,
          "zero exposure should not move timestamp");
  require(slam::exposure_midpoint_timestamp_ns(trigger_ns, -1) == trigger_ns,
          "invalid exposure should not move timestamp");
  require(slam::exposure_midpoint_timestamp_ns(trigger_ns, 2000) ==
              1'001'000'000LL,
          "2000us exposure should add 1000us");
  require(slam::exposure_midpoint_timestamp_ns(trigger_ns, 1) ==
              1'000'000'500LL,
          "1us exposure should add 500ns");

  return 0;
}
