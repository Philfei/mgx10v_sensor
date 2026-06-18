#pragma once

#include <cstdint>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class RecordWriter {
public:
  enum class CloseMode {
    Flush,
    DropPending,
  };

  RecordWriter(std::string data_root, int chunk_duration_s);
  ~RecordWriter();

  RecordWriter(const RecordWriter&) = delete;
  RecordWriter& operator=(const RecordWriter&) = delete;

  void append(const std::string& stream_name, int64_t timestamp_ns,
              std::string header, std::string raw_payload);
  void set_stream_flush_on_close(const std::string& stream_name, bool flush);
  void flush();
  void close(CloseMode mode);

  const std::string& session_dir() const { return session_dir_; }

private:
  struct Record {
    int64_t timestamp_ns = 0;
    std::string header;
    std::string raw_payload;
  };

  struct WorkItem;

  struct StreamState;

  std::shared_ptr<StreamState> get_stream(const std::string& stream_name);
  void wait_stream_flushed(const std::shared_ptr<StreamState>& state,
                           uint64_t flush_id, std::exception_ptr* first_error);
  void worker_loop(std::shared_ptr<StreamState> state);
  void stop_workers(CloseMode mode) noexcept;
  void open_chunk_if_needed(const std::string& stream_dir, const Record& record,
                            std::ofstream* out,
                            int64_t* current_chunk_start_ns) const;
  static void write_record(std::ofstream& out, const Record& record);
  static std::string make_session_dir(const std::string& data_root);
  static std::string chunk_filename(int64_t start_ns, int chunk_duration_s);

  std::string data_root_;
  std::string session_dir_;
  int chunk_duration_s_ = 5;
  int64_t chunk_duration_ns_ = 5'000'000'000LL;
  std::mutex streams_mutex_;
  std::unordered_map<std::string, std::shared_ptr<StreamState>> streams_;
  bool closed_ = false;
};
