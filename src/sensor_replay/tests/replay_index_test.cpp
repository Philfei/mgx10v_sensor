#include "replay_index.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
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

std::vector<uint8_t> timestamp_msg(int64_t sec, int32_t nsec) {
  std::vector<uint8_t> out;
  out.push_back(0x08);
  append_varint(&out, static_cast<uint64_t>(sec));
  out.push_back(0x10);
  append_varint(&out, static_cast<uint32_t>(nsec));
  return out;
}

std::vector<uint8_t> raw_image_header(int64_t sec, int32_t nsec) {
  std::vector<uint8_t> ts = timestamp_msg(sec, nsec);
  std::vector<uint8_t> out;
  out.push_back(0x0a);
  append_varint(&out, ts.size());
  out.insert(out.end(), ts.begin(), ts.end());
  return out;
}

std::vector<uint8_t> imu_header(int64_t sec, int32_t nsec) {
  std::vector<uint8_t> ts = timestamp_msg(sec, nsec);
  std::vector<uint8_t> common_header;
  common_header.push_back(0x0a);
  append_varint(&common_header, ts.size());
  common_header.insert(common_header.end(), ts.begin(), ts.end());

  std::vector<uint8_t> out;
  out.push_back(0x0a);
  append_varint(&out, common_header.size());
  out.insert(out.end(), common_header.begin(), common_header.end());
  return out;
}

void write_u32(std::ofstream& out, uint32_t value) {
  char bytes[4] = {
      static_cast<char>(value & 0xff),
      static_cast<char>((value >> 8) & 0xff),
      static_cast<char>((value >> 16) & 0xff),
      static_cast<char>((value >> 24) & 0xff),
  };
  out.write(bytes, sizeof(bytes));
}

void write_record(const std::filesystem::path& path,
                  const std::vector<uint8_t>& header,
                  const std::string& raw) {
  std::ofstream out(path, std::ios::binary);
  write_u32(out, static_cast<uint32_t>(header.size()));
  out.write(reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
  write_u32(out, static_cast<uint32_t>(raw.size()));
  out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
}

void write_record_with_zero_padding(const std::filesystem::path& path,
                                    const std::vector<uint8_t>& header,
                                    const std::string& raw,
                                    size_t padding_bytes) {
  std::ofstream out(path, std::ios::binary);
  write_u32(out, static_cast<uint32_t>(header.size()));
  out.write(reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
  write_u32(out, static_cast<uint32_t>(raw.size()));
  out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
  std::vector<char> padding(padding_bytes, 0);
  out.write(padding.data(), static_cast<std::streamsize>(padding.size()));
}

}  // namespace

int main() {
  const auto root =
      std::filesystem::temp_directory_path() / "sensor_replay_index_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "cam_left");
  std::filesystem::create_directories(root / "imu");

  write_record(root / "cam_left" / "100_000000000_5s_chunk.dat",
               raw_image_header(100, 200000000), "image");
  write_record(root / "imu" / "100_000000000_5s_chunk.dat",
               imu_header(100, 100000000), "");

  std::vector<ReplayTopic> topics = {
      {"cam_left", ReplaySensorKind::RawImage, "ipc:///tmp/cam_left",
       "cam_left_topic", true},
      {"imu", ReplaySensorKind::Imu, "ipc:///tmp/imu_data", "imu_topic", true},
  };
  const std::vector<ReplayEvent> events = load_replay_events(root, topics);

  require(events.size() == 2, "event count mismatch");
  require(events[0].stream_name == "imu", "events not sorted by timestamp");
  require(events[1].stream_name == "cam_left", "camera event missing");
  require(events[1].raw_size == 5, "camera raw size mismatch");

  const std::string raw = read_event_raw(events[1]);
  require(raw == "image", "camera raw payload mismatch");

  write_record_with_zero_padding(
      root / "cam_left" / "105_000000000_5s_chunk.dat",
      raw_image_header(105, 300000000), "image2", 15);
  const std::vector<ReplayEvent> padded_events =
      load_replay_events(root, topics);
  require(padded_events.size() == 3,
          "zero padded chunk should not create extra events");
  require(padded_events[2].stream_name == "cam_left",
          "padded camera event missing");
  require(read_event_raw(padded_events[2]) == "image2",
          "padded camera raw payload mismatch");

  std::filesystem::remove_all(root);
  return 0;
}
