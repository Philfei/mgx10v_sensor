#pragma once

#include <cstddef>
#include <cstdint>

namespace mgx10v::ipc {

constexpr uint32_t kShmImageMagic = 0x4d475849;  // MGXI
constexpr uint32_t kShmImageVersion = 1;
constexpr uint64_t kShmImageWritingBit = 1ull << 63;
constexpr uint64_t kShmImageHeaderSize = 4096;
constexpr uint64_t kShmImageAlignment = 64;

struct ShmImageHeader {
  uint32_t magic = kShmImageMagic;
  uint32_t version = kShmImageVersion;
  uint32_t slot_count = 0;
  uint32_t reserved = 0;
  uint64_t slot_size = 0;
  uint64_t slot_stride = 0;
  uint64_t total_size = 0;
};

struct ShmImageSlotHeader {
  uint64_t sequence_begin = 0;
  uint64_t sequence_end = 0;
  uint64_t data_size = 0;
  uint64_t write_time_ns = 0;
};

inline uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

inline uint64_t slot_payload_prefix_size() {
  return align_up(sizeof(ShmImageSlotHeader), kShmImageAlignment);
}

inline uint64_t slot_stride_for(uint64_t slot_size) {
  return slot_payload_prefix_size() +
         align_up(slot_size, kShmImageAlignment);
}

inline uint64_t total_size_for(uint32_t slot_count, uint64_t slot_size) {
  return kShmImageHeaderSize +
         static_cast<uint64_t>(slot_count) * slot_stride_for(slot_size);
}

inline uint64_t slot_header_offset(uint32_t slot, uint64_t slot_stride) {
  return kShmImageHeaderSize + static_cast<uint64_t>(slot) * slot_stride;
}

inline uint64_t slot_payload_offset(uint32_t slot, uint64_t slot_stride) {
  return slot_header_offset(slot, slot_stride) + slot_payload_prefix_size();
}

}  // namespace mgx10v::ipc
