#include "replay_timing.h"

#include <cstdint>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  constexpr int64_t first_ns = 1'000'000'000LL;
  constexpr int64_t next_ns = 1'050'000'000LL;

  require(replay_delay_ns(first_ns, first_ns, 0, 1.0) == 0,
          "first event should not sleep");
  require(replay_delay_ns(first_ns, next_ns, 0, 1.0) == 50'000'000LL,
          "full delay mismatch");
  require(replay_delay_ns(first_ns, next_ns, 20'000'000LL, 1.0) ==
              30'000'000LL,
          "processing time was not deducted");
  require(replay_delay_ns(first_ns, next_ns, 70'000'000LL, 1.0) == 0,
          "late replay should not sleep");
  require(replay_delay_ns(first_ns, next_ns, 0, 2.0) == 25'000'000LL,
          "speed scaling mismatch");
  require(replay_delay_ns(first_ns, next_ns, 0, 0.0) == 0,
          "speed zero should not sleep");

  return 0;
}
