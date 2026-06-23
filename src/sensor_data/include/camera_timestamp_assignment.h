#pragma once

#include "exposure_timestamp.h"
#include "types.h"

namespace slam {

inline ns_t assign_camera_mid_exposure_timestamp_ns(ns_t cam_sync_anchor_ns,
                                                    int exposure_us) {
  return exposure_midpoint_timestamp_ns(cam_sync_anchor_ns, exposure_us);
}

}  // namespace slam
