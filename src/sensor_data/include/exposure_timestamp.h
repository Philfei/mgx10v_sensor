#pragma once

#include "types.h"

namespace slam {

inline ns_t exposure_midpoint_timestamp_ns(ns_t trigger_timestamp_ns,
                                           int exposure_us) {
  if (exposure_us <= 0) return trigger_timestamp_ns;
  return trigger_timestamp_ns + static_cast<ns_t>(exposure_us) * 500LL;
}

}  // namespace slam
