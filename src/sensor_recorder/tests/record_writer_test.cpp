#include "record_writer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

uint32_t read_u32_le(const std::vector<char>& bytes, size_t* offset) {
  require(*offset + sizeof(uint32_t) <= bytes.size(), "short uint32 read");
  uint32_t value = 0;
  value |= static_cast<uint8_t>(bytes[(*offset)++]);
  value |= static_cast<uint8_t>(bytes[(*offset)++]) << 8;
  value |= static_cast<uint8_t>(bytes[(*offset)++]) << 16;
  value |= static_cast<uint8_t>(bytes[(*offset)++]) << 24;
  return value;
}

std::vector<char> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  require(in.is_open(), "failed to open chunk file");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
  {
    const auto root = std::filesystem::temp_directory_path() /
                      "sensor_recorder_record_writer_test";
    std::filesystem::remove_all(root);
    std::filesystem::path imu_dir;

    {
      RecordWriter writer(root.string(), 5);
      writer.append("imu", 10'000'000'000LL, "imu-header", {});
      writer.append("imu", 11'000'000'000LL, "imu-header-2", "raw");
      writer.flush();

      const auto base = writer.session_dir();
      imu_dir = std::filesystem::path(base) / "imu";
      require(std::filesystem::exists(imu_dir), "imu directory was not created");

      std::vector<std::filesystem::path> chunks;
      for (const auto& entry : std::filesystem::directory_iterator(imu_dir)) {
        chunks.push_back(entry.path());
      }
      require(chunks.size() == 1, "expected one chunk after first flush");

      const auto bytes = read_file(chunks.front());
      size_t off = 0;
      require(read_u32_le(bytes, &off) == 10, "first header size mismatch");
      require(std::string(bytes.data() + off, bytes.data() + off + 10) ==
                  "imu-header",
              "first header bytes mismatch");
      off += 10;
      require(read_u32_le(bytes, &off) == 0, "first raw size mismatch");

      require(read_u32_le(bytes, &off) == 12, "second header size mismatch");
      require(std::string(bytes.data() + off, bytes.data() + off + 12) ==
                  "imu-header-2",
              "second header bytes mismatch");
      off += 12;
      require(read_u32_le(bytes, &off) == 3, "second raw size mismatch");
      require(std::string(bytes.data() + off, bytes.data() + off + 3) == "raw",
              "second raw bytes mismatch");
      off += 3;
      require(off == bytes.size(), "chunk contains unexpected trailing bytes");

      writer.append("imu", 16'000'000'000LL, "next", {});
      writer.flush();

      chunks.clear();
      for (const auto& entry : std::filesystem::directory_iterator(imu_dir)) {
        chunks.push_back(entry.path());
      }
      require(chunks.size() == 2, "expected second chunk after rollover");
    }

    std::filesystem::remove_all(root);
  }

  {
    const auto root = std::filesystem::temp_directory_path() /
                      "sensor_recorder_record_writer_isolation_test";
    std::filesystem::remove_all(root);

    {
      RecordWriter writer(root.string(), 5);
      const auto cam_dir =
          std::filesystem::path(writer.session_dir()) / "cam_left";
      std::filesystem::create_directories(cam_dir);
      std::filesystem::create_directory(cam_dir / "10_000000000_5s_chunk.dat");

      writer.append("cam_left", 10'000'000'000LL, "cam-header", "raw");
      writer.append("imu", 16'000'000'000LL, "imu-after-camera-error", {});
    }
    std::filesystem::remove_all(root);
  }

  return 0;
}
