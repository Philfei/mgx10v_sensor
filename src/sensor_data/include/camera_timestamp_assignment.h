#pragma once

#include "exposure_timestamp.h"
#include "frame_timestamp_tracker.h"
#include "types.h"

#include <cstdint>

namespace slam {

inline ns_t assign_camera_mid_exposure_timestamp_ns(
    FrameTimestampTracker& tracker,
    uint64_t frame_pts_us,
    ns_t cam_sync_anchor_ns,
    int exposure_us) {
  const ns_t trigger_timestamp_ns =
      tracker.assign(frame_pts_us, cam_sync_anchor_ns);
  return exposure_midpoint_timestamp_ns(trigger_timestamp_ns, exposure_us);
}

}  // namespace slam
