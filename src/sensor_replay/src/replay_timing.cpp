#include "replay_timing.h"

int64_t replay_delay_ns(int64_t first_event_ns, int64_t event_ns,
                        int64_t elapsed_wall_ns, double speed) {
  if (speed <= 0.0 || event_ns <= first_event_ns) {
    return 0;
  }

  const double target_elapsed =
      static_cast<double>(event_ns - first_event_ns) / speed;
  const double delay = target_elapsed - static_cast<double>(elapsed_wall_ns);
  if (delay <= 0.0) {
    return 0;
  }
  return static_cast<int64_t>(delay);
}
