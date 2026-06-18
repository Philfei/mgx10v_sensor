#include "shared_image_reader.h"

#include "shm_image_ring_layout.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

uint64_t atomic_load_u64(const uint64_t* value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

}  // namespace

SharedImageReader::~SharedImageReader() {
  close_all();
}

bool SharedImageReader::copy_image(const mgx10v::proto::RawImage& msg,
                                   std::vector<uint8_t>* out,
                                   bool* overwritten) {
  if (overwritten) {
    *overwritten = false;
  }
  if (!out || msg.shm_name().empty() || msg.data_size() == 0 ||
      msg.sequence() == 0) {
    return false;
  }

  Mapping* mapping = get_mapping(msg.shm_name());
  if (!mapping) {
    return false;
  }

  const uint64_t data_size = msg.data_size();
  const uint64_t offset = msg.shm_offset();
  if (offset > mapping->size || data_size > mapping->size - offset) {
    return false;
  }
  if (msg.shm_slot() >= mapping->slot_count) {
    return false;
  }

  const uint64_t slot_header_pos =
      mgx10v::ipc::slot_header_offset(msg.shm_slot(),
                                      mapping->slot_stride);
  if (slot_header_pos + sizeof(mgx10v::ipc::ShmImageSlotHeader) >
      mapping->size) {
    return false;
  }

  const auto* slot_header =
      reinterpret_cast<const mgx10v::ipc::ShmImageSlotHeader*>(
          static_cast<const uint8_t*>(mapping->base) + slot_header_pos);

  const uint64_t sequence = msg.sequence();
  const uint64_t begin_before =
      atomic_load_u64(&slot_header->sequence_begin);
  const uint64_t end_before = atomic_load_u64(&slot_header->sequence_end);
  const uint64_t slot_size = atomic_load_u64(&slot_header->data_size);
  if (begin_before != sequence || end_before != sequence ||
      slot_size < data_size) {
    if (overwritten) {
      *overwritten = true;
    }
    return false;
  }

  out->resize(static_cast<size_t>(data_size));
  std::memcpy(out->data(),
              static_cast<const uint8_t*>(mapping->base) + offset,
              static_cast<size_t>(data_size));

  const uint64_t begin_after =
      atomic_load_u64(&slot_header->sequence_begin);
  const uint64_t end_after = atomic_load_u64(&slot_header->sequence_end);
  if (begin_after != sequence || end_after != sequence) {
    if (overwritten) {
      *overwritten = true;
    }
    return false;
  }
  return true;
}

SharedImageReader::Mapping* SharedImageReader::get_mapping(
    const std::string& name) {
  auto it = mappings_.find(name);
  if (it != mappings_.end()) {
    return &it->second;
  }

  Mapping mapping;
  mapping.fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (mapping.fd < 0) {
    std::fprintf(stderr, "shm_open %s failed: %s\n", name.c_str(),
                 std::strerror(errno));
    return nullptr;
  }

  struct stat st {};
  if (fstat(mapping.fd, &st) < 0 || st.st_size <= 0) {
    std::fprintf(stderr, "fstat %s failed: %s\n", name.c_str(),
                 std::strerror(errno));
    close_mapping(&mapping);
    return nullptr;
  }
  mapping.size = static_cast<size_t>(st.st_size);
  mapping.base = mmap(nullptr, mapping.size, PROT_READ, MAP_SHARED,
                      mapping.fd, 0);
  if (mapping.base == MAP_FAILED) {
    std::fprintf(stderr, "mmap %s failed: %s\n", name.c_str(),
                 std::strerror(errno));
    mapping.base = nullptr;
    close_mapping(&mapping);
    return nullptr;
  }

  if (!validate_mapping(&mapping)) {
    std::fprintf(stderr, "invalid image shared memory: %s\n", name.c_str());
    close_mapping(&mapping);
    return nullptr;
  }

  auto inserted = mappings_.emplace(name, mapping);
  return &inserted.first->second;
}

bool SharedImageReader::validate_mapping(Mapping* mapping) {
  if (!mapping || mapping->size < mgx10v::ipc::kShmImageHeaderSize) {
    return false;
  }
  const auto* header = reinterpret_cast<const mgx10v::ipc::ShmImageHeader*>(
      mapping->base);
  if (header->magic != mgx10v::ipc::kShmImageMagic ||
      header->version != mgx10v::ipc::kShmImageVersion ||
      header->slot_count == 0 || header->slot_size == 0 ||
      header->slot_stride == 0 || header->total_size > mapping->size) {
    return false;
  }
  mapping->slot_count = header->slot_count;
  mapping->slot_stride = header->slot_stride;
  return true;
}

void SharedImageReader::close_mapping(Mapping* mapping) {
  if (!mapping) {
    return;
  }
  if (mapping->base && mapping->base != MAP_FAILED) {
    munmap(mapping->base, mapping->size);
    mapping->base = nullptr;
  }
  if (mapping->fd >= 0) {
    close(mapping->fd);
    mapping->fd = -1;
  }
  mapping->size = 0;
}

void SharedImageReader::close_all() {
  for (auto& item : mappings_) {
    close_mapping(&item.second);
  }
  mappings_.clear();
}
