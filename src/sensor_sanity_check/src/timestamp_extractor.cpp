#include "timestamp_extractor.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>

namespace {

constexpr int64_t kNsPerSecond = 1'000'000'000LL;

std::optional<int64_t> extract_google_timestamp_ns(
    google::protobuf::io::CodedInputStream* input) {
  int64_t seconds = 0;
  int32_t nanos = 0;
  bool has_seconds = false;

  while (true) {
    const uint32_t tag = input->ReadTag();
    if (tag == 0) {
      break;
    }
    const int field_number =
        google::protobuf::internal::WireFormatLite::GetTagFieldNumber(tag);
    const auto wire_type =
        google::protobuf::internal::WireFormatLite::GetTagWireType(tag);

    if (field_number == 1 &&
        wire_type ==
            google::protobuf::internal::WireFormatLite::WIRETYPE_VARINT) {
      uint64_t value = 0;
      if (!input->ReadVarint64(&value)) {
        return std::nullopt;
      }
      seconds = static_cast<int64_t>(value);
      has_seconds = true;
      continue;
    }
    if (field_number == 2 &&
        wire_type ==
            google::protobuf::internal::WireFormatLite::WIRETYPE_VARINT) {
      uint32_t value = 0;
      if (!input->ReadVarint32(&value)) {
        return std::nullopt;
      }
      nanos = static_cast<int32_t>(value);
      continue;
    }
    if (!google::protobuf::internal::WireFormatLite::SkipField(input, tag)) {
      return std::nullopt;
    }
  }
  if (!has_seconds) {
    return std::nullopt;
  }
  return seconds * kNsPerSecond + static_cast<int64_t>(nanos);
}

std::optional<int64_t> extract_header_stamp_ns(
    google::protobuf::io::CodedInputStream* input) {
  while (true) {
    const uint32_t tag = input->ReadTag();
    if (tag == 0) {
      break;
    }
    const int field_number =
        google::protobuf::internal::WireFormatLite::GetTagFieldNumber(tag);
    const auto wire_type =
        google::protobuf::internal::WireFormatLite::GetTagWireType(tag);
    if (field_number == 1 &&
        wire_type ==
            google::protobuf::internal::WireFormatLite::
                WIRETYPE_LENGTH_DELIMITED) {
      uint32_t length = 0;
      if (!input->ReadVarint32(&length)) {
        return std::nullopt;
      }
      auto limit = input->PushLimit(static_cast<int>(length));
      auto stamp = extract_google_timestamp_ns(input);
      input->PopLimit(limit);
      return stamp;
    }
    if (!google::protobuf::internal::WireFormatLite::SkipField(input, tag)) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<int64_t> extract_length_delimited_timestamp(
    const std::vector<uint8_t>& header, int field_number,
    bool nested_header) {
  google::protobuf::io::CodedInputStream input(header.data(),
                                              static_cast<int>(header.size()));
  while (true) {
    const uint32_t tag = input.ReadTag();
    if (tag == 0) {
      break;
    }
    const int current_field =
        google::protobuf::internal::WireFormatLite::GetTagFieldNumber(tag);
    const auto wire_type =
        google::protobuf::internal::WireFormatLite::GetTagWireType(tag);

    if (current_field == field_number &&
        wire_type ==
            google::protobuf::internal::WireFormatLite::
                WIRETYPE_LENGTH_DELIMITED) {
      uint32_t length = 0;
      if (!input.ReadVarint32(&length)) {
        return std::nullopt;
      }
      auto limit = input.PushLimit(static_cast<int>(length));
      auto stamp =
          nested_header ? extract_header_stamp_ns(&input)
                        : extract_google_timestamp_ns(&input);
      input.PopLimit(limit);
      return stamp;
    }
    if (!google::protobuf::internal::WireFormatLite::SkipField(&input, tag)) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<int64_t> extract_sensor_timestamp_ns(
    const std::vector<uint8_t>& header, SensorKind kind) {
  switch (kind) {
    case SensorKind::RawImage:
      return extract_length_delimited_timestamp(header, 1, false);
    case SensorKind::Imu:
      return extract_length_delimited_timestamp(header, 1, true);
    case SensorKind::Gnss:
      return extract_length_delimited_timestamp(header, 10, true);
  }
  return std::nullopt;
}
