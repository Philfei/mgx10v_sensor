#pragma once
#include <cstdint>
#include <vector>
#include <memory>

namespace slam {

using ns_t = int64_t;

struct Frame {
  ns_t  ts_ns = 0;
  int   width = 0;
  int   height = 0;
  int   stride = 0;
  uint32_t pixfmt = 0;
  uint32_t colorspace = 0;
  uint32_t ycbcr_enc = 0;
  uint32_t quantization = 0;
  std::vector<uint8_t> data;
};

struct ImuSample {
  ns_t   ts_ns = 0;
  double ax = 0, ay = 0, az = 0;
  double gx = 0, gy = 0, gz = 0;
};

struct FramePair {
  std::shared_ptr<Frame> left;
  std::shared_ptr<Frame> right;
  ns_t pair_dt_ns = 0;
};

}
