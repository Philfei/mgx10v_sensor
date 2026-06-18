#include "timestamp_extractor.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void append_varint(std::vector<uint8_t>* out, uint64_t value) {
  while (value >= 0x80) {
    out->push_back(static_cast<uint8_t>(value | 0x80));
    value >>= 7;
  }
  out->push_back(static_cast<uint8_t>(value));
}

}  // namespace

int main() {
  std::vector<uint8_t> timestamp;
  timestamp.push_back(0x08);  // seconds field
  append_varint(&timestamp, 123);
  timestamp.push_back(0x10);  // nanos field
  append_varint(&timestamp, 456000000);

  std::vector<uint8_t> raw_image;
  raw_image.push_back(0x0a);  // RawImage timestamp field 1, length-delimited
  append_varint(&raw_image, timestamp.size());
  raw_image.insert(raw_image.end(), timestamp.begin(), timestamp.end());

  const auto stamp = extract_sensor_timestamp_ns(raw_image, SensorKind::RawImage);
  require(stamp.has_value(), "timestamp was not extracted");
  require(*stamp == 123456000000LL, "timestamp ns mismatch");
  return 0;
}
