#include "replay_index.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace {

constexpr int64_t kNsPerSecond = 1'000'000'000LL;
constexpr uint32_t kMaxHeaderSize = 16u * 1024u * 1024u;
constexpr uint32_t kMaxRawSize = 512u * 1024u * 1024u;

bool ends_with(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_chunk_file(const std::filesystem::path& path) {
  return ends_with(path.filename().string(), "_chunk.dat");
}

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

bool remaining_stream_is_zero(std::ifstream& in) {
  std::array<char, 64 * 1024> buffer{};
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = in.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      if (buffer[static_cast<size_t>(i)] != 0) {
        return false;
      }
    }
  }
  return in.eof();
}

std::vector<std::filesystem::path> collect_chunks(
    const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> files;
  if (!std::filesystem::is_directory(dir)) {
    return files;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && is_chunk_file(entry.path())) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::optional<int64_t> extract_google_timestamp_ns(
    google::protobuf::io::CodedInputStream* input) {
  int64_t seconds = 0;
  int32_t nanos = 0;
  bool has_seconds = false;
  while (true) {
    const uint32_t tag = input->ReadTag();
    if (tag == 0) break;
    const int field =
        google::protobuf::internal::WireFormatLite::GetTagFieldNumber(tag);
    const auto wire =
        google::protobuf::internal::WireFormatLite::GetTagWireType(tag);
    if (field == 1 &&
        wire == google::protobuf::internal::WireFormatLite::WIRETYPE_VARINT) {
      uint64_t value = 0;
      if (!input->ReadVarint64(&value)) return std::nullopt;
      seconds = static_cast<int64_t>(value);
      has_seconds = true;
      continue;
    }
    if (field == 2 &&
        wire == google::protobuf::internal::WireFormatLite::WIRETYPE_VARINT) {
      uint32_t value = 0;
      if (!input->ReadVarint32(&value)) return std::nullopt;
      nanos = static_cast<int32_t>(value);
      continue;
    }
    if (!google::protobuf::internal::WireFormatLite::SkipField(input, tag)) {
      return std::nullopt;
    }
  }
  if (!has_seconds) return std::nullopt;
  return seconds * kNsPerSecond + static_cast<int64_t>(nanos);
}

std::optional<int64_t> extract_header_stamp_ns(
    google::protobuf::io::CodedInputStream* input) {
  while (true) {
    const uint32_t tag = input->ReadTag();
    if (tag == 0) break;
    const int field =
        google::protobuf::internal::WireFormatLite::GetTagFieldNumber(tag);
    const auto wire =
        google::protobuf::internal::WireFormatLite::GetTagWireType(tag);
    if (field == 1 &&
        wire == google::protobuf::internal::WireFormatLite::
                    WIRETYPE_LENGTH_DELIMITED) {
      uint32_t length = 0;
      if (!input->ReadVarint32(&length)) return std::nullopt;
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
    const std::vector<uint8_t>& header, int field_number, bool nested_header) {
  google::protobuf::io::CodedInputStream input(header.data(),
                                              static_cast<int>(header.size()));
  while (true) {
    const uint32_t tag = input.ReadTag();
    if (tag == 0) break;
    const int field =
        google::protobuf::internal::WireFormatLite::GetTagFieldNumber(tag);
    const auto wire =
        google::protobuf::internal::WireFormatLite::GetTagWireType(tag);
    if (field == field_number &&
        wire == google::protobuf::internal::WireFormatLite::
                    WIRETYPE_LENGTH_DELIMITED) {
      uint32_t length = 0;
      if (!input.ReadVarint32(&length)) return std::nullopt;
      auto limit = input.PushLimit(static_cast<int>(length));
      auto stamp = nested_header ? extract_header_stamp_ns(&input)
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

std::optional<int64_t> extract_timestamp_ns(const std::vector<uint8_t>& header,
                                            ReplaySensorKind kind) {
  switch (kind) {
    case ReplaySensorKind::RawImage:
      return extract_length_delimited_timestamp(header, 1, false);
    case ReplaySensorKind::Imu:
      return extract_length_delimited_timestamp(header, 1, true);
    case ReplaySensorKind::Gnss:
      return extract_length_delimited_timestamp(header, 10, true);
  }
  return std::nullopt;
}

}  // namespace

std::vector<ReplayEvent> load_replay_events(
    const std::filesystem::path& dataset_dir,
    const std::vector<ReplayTopic>& topics) {
  std::vector<ReplayEvent> events;
  for (const ReplayTopic& topic : topics) {
    if (!topic.enabled) {
      continue;
    }
    for (const auto& file_path : collect_chunks(dataset_dir / topic.name)) {
      const uint64_t file_size = std::filesystem::file_size(file_path);
      std::ifstream in(file_path, std::ios::binary);
      if (!in.is_open()) {
        throw std::runtime_error("cannot open chunk: " + file_path.string());
      }
      while (true) {
        uint32_t header_size = 0;
        if (!read_u32_le(in, &header_size)) {
          if (in.eof()) break;
          throw std::runtime_error("failed to read header size");
        }
        if (header_size > kMaxHeaderSize) {
          throw std::runtime_error("header too large in " + file_path.string());
        }
        if (header_size == 0) {
          if (remaining_stream_is_zero(in)) {
            break;
          }
          throw std::runtime_error("zero header before nonzero data in " +
                                   file_path.string());
        }
        std::vector<uint8_t> header(header_size);
        if (header_size > 0) {
          in.read(reinterpret_cast<char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
          if (!in) {
            if (in.eof()) break;
            throw std::runtime_error("incomplete header in " + file_path.string());
          }
        }
        uint32_t raw_size = 0;
        if (!read_u32_le(in, &raw_size)) {
          if (in.eof()) break;
          throw std::runtime_error("missing raw size in " + file_path.string());
        }
        if (raw_size > kMaxRawSize) {
          throw std::runtime_error("raw payload too large in " +
                                   file_path.string());
        }
        const auto raw_offset_pos = in.tellg();
        if (raw_offset_pos < 0) {
          throw std::runtime_error("cannot locate raw payload in " +
                                   file_path.string());
        }
        const uint64_t raw_offset = static_cast<uint64_t>(raw_offset_pos);
        if (raw_offset + raw_size > file_size) {
          break;
        }
        auto timestamp = extract_timestamp_ns(header, topic.kind);
        in.seekg(static_cast<std::streamoff>(raw_size), std::ios::cur);
        if (!in) {
          throw std::runtime_error("incomplete raw payload in " +
                                   file_path.string());
        }
        if (timestamp.has_value()) {
          events.push_back({*timestamp,
                            topic.name,
                            topic.kind,
                            topic.endpoint,
                            topic.topic,
                            file_path,
                            std::move(header),
                            raw_offset,
                            raw_size});
        }
      }
    }
  }
  std::sort(events.begin(), events.end(),
            [](const ReplayEvent& a, const ReplayEvent& b) {
              if (a.timestamp_ns != b.timestamp_ns) {
                return a.timestamp_ns < b.timestamp_ns;
              }
              return a.stream_name < b.stream_name;
            });
  return events;
}

std::string read_event_raw(const ReplayEvent& event) {
  if (event.raw_size == 0) {
    return {};
  }
  std::ifstream in(event.file_path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("cannot open chunk: " + event.file_path.string());
  }
  in.seekg(static_cast<std::streamoff>(event.raw_offset), std::ios::beg);
  if (!in) {
    throw std::runtime_error("cannot seek raw payload");
  }
  std::string raw(event.raw_size, '\0');
  in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
  if (!in) {
    throw std::runtime_error("incomplete raw payload");
  }
  return raw;
}
