#include "chunk_reader.h"

#include <array>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

constexpr uint32_t kMaxHeaderSize = 16u * 1024u * 1024u;
constexpr uint32_t kMaxRawSize = 512u * 1024u * 1024u;

bool read_u32_le(std::ifstream& in, uint32_t* value) {
  char bytes[4] = {};
  in.read(bytes, sizeof(bytes));
  if (!in) {
    return false;
  }
  *value = static_cast<uint8_t>(bytes[0]) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[3])) << 24);
  return true;
}

bool consume_remaining_zero_bytes(std::ifstream& in, size_t* bytes) {
  std::array<char, 64 * 1024> buffer{};
  *bytes = 0;
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = in.gcount();
    *bytes += static_cast<size_t>(count);
    for (std::streamsize i = 0; i < count; ++i) {
      if (buffer[static_cast<size_t>(i)] != 0) {
        return false;
      }
    }
  }
  return in.eof();
}

void seek_forward(std::ifstream& in, uint32_t bytes) {
  if (bytes == 0) {
    return;
  }
  in.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
  if (!in) {
    throw std::runtime_error("incomplete raw payload");
  }
}

}  // namespace

ChunkReadStats for_each_record_header(const std::filesystem::path& path,
                                      const RecordHeaderCallback& callback) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("cannot open chunk: " + path.string());
  }

  ChunkReadStats stats;
  std::vector<uint8_t> header;
  while (true) {
    uint32_t header_size = 0;
    if (!read_u32_le(in, &header_size)) {
      if (in.eof()) {
        break;
      }
      throw std::runtime_error("failed to read header size");
    }
    stats.bytes += sizeof(uint32_t);
    if (header_size > kMaxHeaderSize) {
      throw std::runtime_error("header too large");
    }
    if (header_size == 0) {
      size_t padding_bytes = 0;
      if (consume_remaining_zero_bytes(in, &padding_bytes)) {
        stats.bytes += padding_bytes;
        break;
      }
      throw std::runtime_error("zero header before nonzero data");
    }

    header.resize(header_size);
    if (header_size > 0) {
      in.read(reinterpret_cast<char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
      if (!in) {
        throw std::runtime_error("incomplete header payload");
      }
    }
    stats.bytes += header_size;

    uint32_t raw_size = 0;
    if (!read_u32_le(in, &raw_size)) {
      throw std::runtime_error("missing raw size");
    }
    stats.bytes += sizeof(uint32_t);
    if (raw_size > kMaxRawSize) {
      throw std::runtime_error("raw payload too large");
    }

    callback(header, raw_size);
    seek_forward(in, raw_size);
    stats.bytes += raw_size;
    ++stats.records;
  }
  return stats;
}
