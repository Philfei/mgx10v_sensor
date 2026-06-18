#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

struct ChunkReadStats {
  uint64_t records = 0;
  uint64_t bytes = 0;
};

using RecordHeaderCallback =
    std::function<void(const std::vector<uint8_t>& header, uint32_t raw_size)>;

ChunkReadStats for_each_record_header(const std::filesystem::path& path,
                                      const RecordHeaderCallback& callback);
