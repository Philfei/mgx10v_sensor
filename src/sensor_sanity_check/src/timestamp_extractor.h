#pragma once

#include <cstdint>
#include <optional>
#include <vector>

enum class SensorKind {
  RawImage,
  Imu,
  Gnss,
};

std::optional<int64_t> extract_sensor_timestamp_ns(
    const std::vector<uint8_t>& header, SensorKind kind);
