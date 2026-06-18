#pragma once

#include "types.h"

#include <cstdint>

namespace slam {

class FrameTimestampTracker {
public:
  ns_t assign(uint64_t frame_pts_us, ns_t anchor_ns) {
    if (!initialized_) {
      last_pts_us_ = frame_pts_us;
      last_timestamp_ns_ = anchor_ns > 0
                               ? anchor_ns
                               : static_cast<ns_t>(frame_pts_us) * 1000LL;
      initialized_ = true;
      return last_timestamp_ns_;
    }

    ns_t timestamp_ns = 0;
    if (last_pts_us_ > 0 && frame_pts_us > last_pts_us_) {
      const uint64_t delta_us = frame_pts_us - last_pts_us_;
      if (delta_us <= kMaxReasonablePtsDeltaUs) {
        timestamp_ns = last_timestamp_ns_ +
                       static_cast<ns_t>(delta_us) * 1000LL;
      }
    }

    if (timestamp_ns == 0 && anchor_ns > last_timestamp_ns_) {
      timestamp_ns = anchor_ns;
    }
    if (timestamp_ns == 0) {
      timestamp_ns = last_timestamp_ns_;
    }

    if (frame_pts_us > 0) {
      last_pts_us_ = frame_pts_us;
    }
    last_timestamp_ns_ = timestamp_ns;
    return timestamp_ns;
  }

private:
  static constexpr uint64_t kMaxReasonablePtsDeltaUs = 1'000'000ULL;

  bool initialized_ = false;
  uint64_t last_pts_us_ = 0;
  ns_t last_timestamp_ns_ = 0;
};

}  // namespace slam
