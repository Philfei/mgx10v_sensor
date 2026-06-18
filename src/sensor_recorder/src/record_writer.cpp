#include "record_writer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

constexpr int64_t kNsPerSecond = 1'000'000'000LL;

void write_u32_le(std::ofstream& out, uint32_t value) {
  const char bytes[4] = {
      static_cast<char>(value & 0xff),
      static_cast<char>((value >> 8) & 0xff),
      static_cast<char>((value >> 16) & 0xff),
      static_cast<char>((value >> 24) & 0xff),
  };
  out.write(bytes, sizeof(bytes));
}

uint32_t checked_size(const std::string& bytes) {
  if (bytes.size() > UINT32_MAX) {
    throw std::runtime_error("record payload exceeds uint32 size limit");
  }
  return static_cast<uint32_t>(bytes.size());
}

}  // namespace

struct RecordWriter::WorkItem {
  enum class Kind {
    Record,
    Flush,
  };

  Kind kind = Kind::Record;
  Record record;
  uint64_t flush_id = 0;
};

struct RecordWriter::StreamState {
  explicit StreamState(std::string stream_dir) : dir(std::move(stream_dir)) {}

  std::string dir;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<WorkItem> pending;
  size_t active_writes = 0;
  uint64_t next_flush_id = 1;
  uint64_t completed_flush_id = 0;
  bool stop = false;
  bool flush_on_close = true;
  std::exception_ptr error;
  std::thread worker;
};

RecordWriter::RecordWriter(std::string data_root, int chunk_duration_s)
    : data_root_(std::move(data_root)),
      session_dir_(make_session_dir(data_root_)),
      chunk_duration_s_(std::max(1, chunk_duration_s)),
      chunk_duration_ns_(static_cast<int64_t>(chunk_duration_s_) *
                         kNsPerSecond) {
  std::filesystem::create_directories(session_dir_);
}

RecordWriter::~RecordWriter() {
  stop_workers(CloseMode::Flush);
}

void RecordWriter::append(const std::string& stream_name, int64_t timestamp_ns,
                          std::string header, std::string raw_payload) {
  auto state = get_stream(stream_name);
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->stop) {
    throw std::runtime_error("record writer stream is stopping: " + stream_name);
  }
  if (state->error) {
    std::rethrow_exception(state->error);
  }

  WorkItem item;
  item.kind = WorkItem::Kind::Record;
  item.record = Record{timestamp_ns, std::move(header), std::move(raw_payload)};
  state->pending.push_back(std::move(item));
  state->cv.notify_one();
}

void RecordWriter::set_stream_flush_on_close(const std::string& stream_name,
                                             bool flush) {
  auto state = get_stream(stream_name);
  std::lock_guard<std::mutex> lock(state->mutex);
  state->flush_on_close = flush;
}

void RecordWriter::flush() {
  std::vector<std::pair<std::shared_ptr<StreamState>, uint64_t>> streams;
  {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    streams.reserve(streams_.size());
    for (const auto& item : streams_) {
      streams.push_back({item.second, 0});
    }
  }

  std::exception_ptr first_error;
  for (auto& item : streams) {
    const auto& state = item.first;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->error) {
      if (!first_error) {
        first_error = state->error;
      }
      continue;
    }
    WorkItem flush_item;
    flush_item.kind = WorkItem::Kind::Flush;
    flush_item.flush_id = state->next_flush_id++;
    item.second = flush_item.flush_id;
    state->pending.push_back(std::move(flush_item));
    state->cv.notify_one();
  }

  for (const auto& item : streams) {
    if (item.second == 0) {
      continue;
    }
    wait_stream_flushed(item.first, item.second, &first_error);
  }
  if (first_error) {
    std::rethrow_exception(first_error);
  }
}

void RecordWriter::close(CloseMode mode) {
  std::exception_ptr first_error;
  if (mode == CloseMode::Flush) {
    try {
      flush();
    } catch (...) {
      first_error = std::current_exception();
    }
  }
  stop_workers(mode);
  if (first_error) {
    std::rethrow_exception(first_error);
  }
}

std::shared_ptr<RecordWriter::StreamState> RecordWriter::get_stream(
    const std::string& stream_name) {
  std::lock_guard<std::mutex> lock(streams_mutex_);
  if (closed_) {
    throw std::runtime_error("record writer is closed");
  }
  auto it = streams_.find(stream_name);
  if (it != streams_.end()) {
    return it->second;
  }

  const std::filesystem::path path =
      std::filesystem::path(session_dir_) / stream_name;
  std::filesystem::create_directories(path);
  auto state = std::make_shared<StreamState>(path.string());
  state->worker = std::thread(&RecordWriter::worker_loop, this, state);
  streams_.emplace(stream_name, state);
  return state;
}

void RecordWriter::wait_stream_flushed(
    const std::shared_ptr<StreamState>& state, uint64_t flush_id,
    std::exception_ptr* first_error) {
  std::unique_lock<std::mutex> lock(state->mutex);
  state->cv.wait(lock, [&]() {
    return state->completed_flush_id >= flush_id;
  });
  if (state->error && first_error && !*first_error) {
    *first_error = state->error;
  }
}

void RecordWriter::worker_loop(std::shared_ptr<StreamState> state) {
  std::ofstream out;
  int64_t current_chunk_start_ns = 0;

  while (true) {
    WorkItem item;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait(lock, [&]() {
        return state->stop || !state->pending.empty();
      });
      if (state->pending.empty()) {
        if (state->stop) {
          break;
        }
        continue;
      }
      item = std::move(state->pending.front());
      state->pending.pop_front();
      ++state->active_writes;
    }

    try {
      if (item.kind == WorkItem::Kind::Record) {
        open_chunk_if_needed(state->dir, item.record, &out,
                             &current_chunk_start_ns);
        write_record(out, item.record);
      } else {
        if (out.is_open()) {
          out.flush();
          if (!out) {
            throw std::runtime_error("failed to flush chunk file in " +
                                     state->dir);
          }
        }
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->error) {
        state->error = std::current_exception();
      }
    }

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (item.kind == WorkItem::Kind::Flush) {
        state->completed_flush_id =
            std::max(state->completed_flush_id, item.flush_id);
      }
      --state->active_writes;
      state->cv.notify_all();
    }
  }

  if (out.is_open()) {
    out.close();
  }
}

void RecordWriter::stop_workers(CloseMode mode) noexcept {
  std::unordered_map<std::string, std::shared_ptr<StreamState>> streams;
  {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
    streams.swap(streams_);
  }

  for (const auto& item : streams) {
    const auto& state = item.second;
    try {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (mode == CloseMode::DropPending && !state->flush_on_close) {
        state->pending.clear();
      }
      state->stop = true;
    } catch (...) {
      try {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stop = true;
      } catch (...) {
      }
    }
    state->cv.notify_all();
  }

  for (const auto& item : streams) {
    const auto& state = item.second;
    if (state->worker.joinable()) {
      state->worker.join();
    }
  }
}

void RecordWriter::open_chunk_if_needed(
    const std::string& stream_dir, const Record& record, std::ofstream* out,
    int64_t* current_chunk_start_ns) const {
  if (!out || !current_chunk_start_ns) {
    throw std::runtime_error("invalid output stream");
  }

  const bool need_new_chunk =
      !out->is_open() ||
      (record.timestamp_ns > 0 &&
       record.timestamp_ns - *current_chunk_start_ns >= chunk_duration_ns_);
  if (!need_new_chunk) {
    return;
  }

  if (out->is_open()) {
    out->close();
    if (!*out) {
      throw std::runtime_error("failed to close chunk file in " + stream_dir);
    }
  }

  *current_chunk_start_ns = record.timestamp_ns > 0 ? record.timestamp_ns : 0;
  const std::filesystem::path path =
      std::filesystem::path(stream_dir) /
      chunk_filename(*current_chunk_start_ns, chunk_duration_s_);
  out->open(path, std::ios::binary | std::ios::out);
  if (!out->is_open()) {
    throw std::runtime_error("failed to open chunk file: " + path.string());
  }
}

void RecordWriter::write_record(std::ofstream& out, const Record& record) {
  write_u32_le(out, checked_size(record.header));
  out.write(record.header.data(),
            static_cast<std::streamsize>(record.header.size()));
  write_u32_le(out, checked_size(record.raw_payload));
  out.write(record.raw_payload.data(),
            static_cast<std::streamsize>(record.raw_payload.size()));
  if (!out) {
    throw std::runtime_error("failed to write record");
  }
}

std::string RecordWriter::make_session_dir(const std::string& data_root) {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &time);
#else
  localtime_r(&time, &tm);
#endif

  std::ostringstream name;
  name << "data_" << std::put_time(&tm, "%Y-%m-%d-%H-%M-%S");
  return (std::filesystem::path(data_root.empty() ? "." : data_root) /
          name.str())
      .string();
}

std::string RecordWriter::chunk_filename(int64_t start_ns,
                                         int chunk_duration_s) {
  if (start_ns <= 0) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now)
                   .count();
  }
  const int64_t seconds = start_ns / kNsPerSecond;
  const int64_t nanos = start_ns % kNsPerSecond;
  std::ostringstream out;
  out << seconds << "_" << std::setfill('0') << std::setw(9) << nanos << "_"
      << chunk_duration_s << "s_chunk.dat";
  return out.str();
}
