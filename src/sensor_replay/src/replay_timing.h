#pragma once

#include <cstdint>

int64_t replay_delay_ns(int64_t first_event_ns, int64_t event_ns,
                        int64_t elapsed_wall_ns, double speed);
