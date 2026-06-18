#include "chunk_reader.h"

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

void write_u32_le(std::ofstream& out, uint32_t value) {
  char bytes[4] = {
      static_cast<char>(value & 0xff),
      static_cast<char>((value >> 8) & 0xff),
      static_cast<char>((value >> 16) & 0xff),
      static_cast<char>((value >> 24) & 0xff),
  };
  out.write(bytes, sizeof(bytes));
}

void write_record(std::ofstream& out, const std::string& header,
                  const std::string& raw) {
  write_u32_le(out, static_cast<uint32_t>(header.size()));
  out.write(header.data(), static_cast<std::streamsize>(header.size()));
  write_u32_le(out, static_cast<uint32_t>(raw.size()));
  out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
}

}  // namespace

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "sensor_sanity_chunk_test.dat";
  {
    std::ofstream out(path, std::ios::binary);
    write_record(out, "header-a", std::string(16, 'x'));
    write_record(out, "header-b", {});
    out.write(std::string(15, '\0').data(), 15);
  }

  std::vector<std::string> headers;
  std::vector<uint32_t> raw_sizes;
  const ChunkReadStats stats = for_each_record_header(
      path, [&](const std::vector<uint8_t>& header, uint32_t raw_size) {
        headers.emplace_back(reinterpret_cast<const char*>(header.data()),
                             header.size());
        raw_sizes.push_back(raw_size);
      });

  require(stats.records == 2, "record count mismatch");
  require(stats.bytes == std::filesystem::file_size(path), "byte count mismatch");
  require(headers.size() == 2, "callback count mismatch");
  require(headers[0] == "header-a", "first header mismatch");
  require(headers[1] == "header-b", "second header mismatch");
  require(raw_sizes[0] == 16, "first raw size mismatch");
  require(raw_sizes[1] == 0, "second raw size mismatch");

  std::filesystem::remove(path);
  return 0;
}
