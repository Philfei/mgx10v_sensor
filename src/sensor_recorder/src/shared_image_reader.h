#pragma once

#include "RawImageMsg.pb.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class SharedImageReader {
public:
  SharedImageReader() = default;
  ~SharedImageReader();

  SharedImageReader(const SharedImageReader&) = delete;
  SharedImageReader& operator=(const SharedImageReader&) = delete;

  bool copy_image(const mgx10v::proto::RawImage& msg,
                  std::vector<uint8_t>* out, bool* overwritten);

private:
  struct Mapping {
    int fd = -1;
    void* base = nullptr;
    size_t size = 0;
    uint32_t slot_count = 0;
    uint64_t slot_stride = 0;
  };

  Mapping* get_mapping(const std::string& name);
  bool validate_mapping(Mapping* mapping);
  void close_mapping(Mapping* mapping);
  void close_all();

  std::unordered_map<std::string, Mapping> mappings_;
};
